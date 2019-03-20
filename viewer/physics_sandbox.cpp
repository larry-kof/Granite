/* Copyright (c) 2017-2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "application.hpp"
#include "os_filesystem.hpp"
#include "scene.hpp"
#include "device.hpp"
#include "mesh_util.hpp"
#include "renderer.hpp"
#include "render_context.hpp"
#include "render_components.hpp"
#include "global_managers.hpp"
#include "physics_system.hpp"
#include "muglm/matrix_helper.hpp"
#include "gltf.hpp"

using namespace Granite;

struct PhysicsSandboxApplication : Application, EventHandler
{
	PhysicsSandboxApplication(const std::string &gltf_path_)
		: renderer(RendererType::GeneralForward), gltf_path(gltf_path_)
	{
		camera.set_position(vec3(0.0f, 2.0f, 8.0f));
		cube = Util::make_handle<CubeMesh>();
		sphere = Util::make_handle<SphereMesh>();
		cone = Util::make_handle<ConeMesh>(16, 1.0f, 0.5f);
		cylinder = Util::make_handle<CylinderMesh>(16, 1.0f, 0.5f);
		init_plane();
		init_scene();
		EVENT_MANAGER_REGISTER_LATCH(PhysicsSandboxApplication, on_swapchain_created, on_swapchain_destroyed, Vulkan::SwapchainParameterEvent);
		EVENT_MANAGER_REGISTER(PhysicsSandboxApplication, on_key, KeyboardEvent);
		EVENT_MANAGER_REGISTER(PhysicsSandboxApplication, on_collision, CollisionEvent);
		EVENT_MANAGER_REGISTER(PhysicsSandboxApplication, on_mouse, MouseButtonEvent);
	}

	bool on_mouse(const MouseButtonEvent &e)
	{
		if (e.get_pressed() && e.get_button() == MouseButton::Left)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity)
			{
				Global::physics()->apply_impulse(result.handle,
						20.0f * camera.get_front(), result.world_pos);
			}
		}

		return true;
	}

	bool on_collision(const CollisionEvent &e)
	{
		auto pos = e.get_world_contact();
		auto n = e.get_world_normal();

		LOGI("Pos: %f, %f, %f\n", pos.x, pos.y, pos.z);
		LOGI("N: %f, %f, %f\n", n.x, n.y, n.z);

		return true;
	}

	void on_swapchain_created(const Vulkan::SwapchainParameterEvent &swap)
	{
		camera.set_aspect(swap.get_aspect_ratio());
		camera.set_fovy(0.4f * pi<float>());
		camera.set_depth_range(0.1f, 500.0f);
	}

	void on_swapchain_destroyed(const Vulkan::SwapchainParameterEvent &)
	{
	}

	void init_plane()
	{
		SceneFormats::Mesh mesh;
		mesh.count = 4;

		const vec3 positions[4] = {
			vec3(-1000.0f, 0.0f, -1000.0f),
			vec3(-1000.0f, 0.0f, +1000.0f),
			vec3(+1000.0f, 0.0f, -1000.0f),
			vec3(+1000.0f, 0.0f, +1000.0f),
		};

		const vec2 uvs[4] = {
			vec2(-1000.0f, -1000.0f),
			vec2(-1000.0f, +1000.0f),
			vec2(+1000.0f, -1000.0f),
			vec2(+1000.0f, +1000.0f),
		};

		mesh.positions.resize(sizeof(positions));
		memcpy(mesh.positions.data(), positions, sizeof(positions));
		mesh.attributes.resize(sizeof(uvs));
		memcpy(mesh.attributes.data(), uvs, sizeof(uvs));
		mesh.position_stride = sizeof(vec3);
		mesh.attribute_stride = sizeof(vec2);
		mesh.attribute_layout[Util::ecast(MeshAttribute::Position)].format = VK_FORMAT_R32G32B32_SFLOAT;
		mesh.attribute_layout[Util::ecast(MeshAttribute::UV)].format = VK_FORMAT_R32G32_SFLOAT;
		mesh.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		mesh.has_material = true;
		mesh.material_index = 0;
		mesh.static_aabb = AABB(vec3(-1000.0f, -1.0f, -1000.0f), vec3(+1000.0f, 0.0f, +1000.0f));

		SceneFormats::MaterialInfo info;
		info.pipeline = DrawPipeline::Opaque;
		info.base_color.path = "builtin://textures/checkerboard.png";
		info.bandlimited_pixel = true;
		info.uniform_roughness = 1.0f;
		info.uniform_metallic = 0.0f;
		plane = Util::make_handle<ImportedMesh>(mesh, info);
	}

	void init_scene()
	{
		auto root_node = scene.create_node();
		auto *entity = scene.create_renderable(plane, root_node.get());
		auto *plane = Global::physics()->add_infinite_plane(vec4(0.0f, 1.0f, 0.0f, 0.0f), {});
		entity->allocate_component<PhysicsComponent>()->handle = plane;
		PhysicsSystem::set_handle_parent(plane, entity);
		scene.set_root_node(root_node);
		context.set_lighting_parameters(&lighting);

		GLTF::Parser parser(gltf_path);
		auto &mesh = parser.get_meshes().front();
		auto *model = scene.create_entity();
		auto &collision_mesh = model->allocate_component<CollisionMeshComponent>()->mesh;

		if (SceneFormats::extract_collision_mesh(collision_mesh, mesh))
		{
			PhysicsSystem::CollisionMesh c;
			c.indices = collision_mesh.indices.data();
			c.num_triangles = collision_mesh.indices.size() / 3;
			c.index_stride_triangle = 3 * sizeof(uint32_t);
			c.num_vertices = collision_mesh.positions.size();
			c.positions = collision_mesh.positions.front().data;
			c.position_stride = sizeof(vec4);
			c.aabb = mesh.static_aabb;
			gltf_mesh_physics_index = Global::physics()->register_collision_mesh(c);
		}

		if (mesh.has_material)
		{
			gltf_mesh = Util::make_handle<ImportedMesh>(mesh,
			                                            parser.get_materials()[mesh.material_index]);
		}
		else
		{
			SceneFormats::MaterialInfo default_material;
			default_material.uniform_base_color = vec4(0.3f, 1.0f, 0.3f, 1.0f);
			default_material.uniform_metallic = 0.0f;
			default_material.uniform_roughness = 1.0f;
			gltf_mesh = Util::make_handle<ImportedMesh>(mesh, default_material);
		}

		{
			auto camera_node = scene.create_node();
			root_node->add_child(camera_node);
			auto *camera_entity = scene.create_entity();
			auto *phys = camera_entity->allocate_component<PhysicsComponent>();
			PhysicsSystem::MaterialInfo info;
			info.type = PhysicsSystem::ObjectType::Kinematic;
			phys->handle = Global::physics()->add_sphere(camera_node.get(), info);
			camera_handle = phys->handle;
		}
	}

	bool on_key(const KeyboardEvent &e)
	{
		if (e.get_key() == Key::M)
			apply_anti_gravity = e.get_key_state() != KeyState::Released;

		if (e.get_key() == Key::Space && e.get_key_state() == KeyState::Pressed)
		{
			auto &handles = scene.get_entity_pool().get_component_group<PhysicsComponent>();
			for (auto &handle : handles)
			{
				auto *h = get_component<PhysicsComponent>(handle);
				if (!PhysicsSystem::get_scene_node(h->handle))
					continue;

				Global::physics()->apply_impulse(h->handle,
				                                 vec3(0.0f, 22.0f, -4.0f),
				                                 vec3(0.2f, 0.0f, 0.0f));
			}
		}
		else if (e.get_key() == Key::R && e.get_key_state() == KeyState::Pressed)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity && PhysicsSystem::get_scene_node(result.handle))
			{
				auto *node = PhysicsSystem::get_scene_node(result.handle);
				if (node && node->get_children().empty())
					Scene::Node::remove_node_from_hierarchy(node);
				scene.destroy_entity(result.entity);
			}
		}
		else if (e.get_key() == Key::O && e.get_key_state() == KeyState::Pressed)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity)
			{
				auto cube_node = scene.create_node();
				cube_node->transform.translation = result.world_pos + vec3(0.0f, 20.0f, 0.0f);
				cube_node->invalidate_cached_transform();
				scene.get_root_node()->add_child(cube_node);
				auto *entity = scene.create_renderable(cube, cube_node.get());
				PhysicsSystem::MaterialInfo info;
				info.mass = 10.0f;
				info.restitution = 0.05f;
				info.angular_damping = 0.3f;
				info.linear_damping = 0.3f;
				auto *cube = Global::physics()->add_cube(cube_node.get(), info);
				entity->allocate_component<PhysicsComponent>()->handle = cube;
				PhysicsSystem::set_handle_parent(cube, entity);
			}
		}
		else if (e.get_key() == Key::L && e.get_key_state() == KeyState::Pressed)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity)
			{
				auto mesh_node = scene.create_node();
				mesh_node->transform.translation = result.world_pos + vec3(0.0f, 1.0f, 0.0f);
				mesh_node->invalidate_cached_transform();
				scene.get_root_node()->add_child(mesh_node);
				auto *entity = scene.create_renderable(gltf_mesh, mesh_node.get());
				auto *mesh = Global::physics()->add_mesh(mesh_node.get(), gltf_mesh_physics_index, {});
				entity->allocate_component<PhysicsComponent>()->handle = mesh;
				PhysicsSystem::set_handle_parent(mesh, entity);
			}
		}
		else if (e.get_key() == Key::K && e.get_key_state() == KeyState::Pressed)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity)
			{
				PhysicsSystem::MaterialInfo info;
				info.mass = 10.0f;
				info.restitution = 0.05f;
				info.angular_damping = 0.3f;
				info.linear_damping = 0.3f;

				auto cube_node_left = scene.create_node();
				cube_node_left->transform.translation = result.world_pos + vec3(0.0f, 20.0f, 0.0f);
				cube_node_left->invalidate_cached_transform();
				scene.get_root_node()->add_child(cube_node_left);
				auto *entity_left = scene.create_renderable(cube, cube_node_left.get());
				auto *cube_left = Global::physics()->add_cube(cube_node_left.get(), info);
				entity_left->allocate_component<PhysicsComponent>()->handle = cube_left;
				PhysicsSystem::set_handle_parent(cube_left, entity_left);

				auto cube_left_hinge = scene.create_node();
				cube_node_left->add_child(cube_left_hinge);
				cube_left_hinge->transform.scale = vec3(0.75f, 0.1f, 0.1f);
				cube_left_hinge->transform.translation = vec3(1.75f, 0.0f, 0.0f);
				scene.create_renderable(cube, cube_left_hinge.get());

				auto cube_node_right = scene.create_node();
				cube_node_right->transform.translation = result.world_pos + vec3(5.0f, 20.0f, 0.0f);
				cube_node_right->invalidate_cached_transform();
				scene.get_root_node()->add_child(cube_node_right);
				auto *entity_right = scene.create_renderable(cube, cube_node_right.get());
				auto *cube_right = Global::physics()->add_cube(cube_node_right.get(), info);
				entity_right->allocate_component<PhysicsComponent>()->handle = cube_right;
				PhysicsSystem::set_handle_parent(cube_right, entity_right);

				auto cube_right_hinge = scene.create_node();
				cube_node_right->add_child(cube_right_hinge);
				cube_right_hinge->transform.scale = vec3(0.75f, 0.1f, 0.1f);
				cube_right_hinge->transform.translation = vec3(-1.75f, 0.0f, 0.0f);
				scene.create_renderable(cube, cube_right_hinge.get());

				Global::physics()->add_point_constraint(cube_left, cube_right,
				                                        vec3(2.5f, 0.0f, 0.0f),
				                                        vec3(-2.5f, 0.0f, 0.0f));
			}
		}
		else if (e.get_key() == Key::P && e.get_key_state() == KeyState::Pressed)
		{
			auto result = Global::physics()->query_closest_hit_ray(
					camera.get_position(), camera.get_front(), 100.0f);

			if (result.entity)
			{
				auto sphere_node = scene.create_node();
				sphere_node->transform.translation = result.world_pos + vec3(0.0f, 20.0f, 0.0f);
				sphere_node->invalidate_cached_transform();
				scene.get_root_node()->add_child(sphere_node);
				auto *entity = scene.create_renderable(cylinder, sphere_node.get());
				PhysicsSystem::MaterialInfo info;
				info.mass = 30.0f;
				info.restitution = 0.2f;
				info.angular_damping = 0.3f;
				info.linear_damping = 0.3f;
				auto *sphere = Global::physics()->add_cylinder(sphere_node.get(), 1.0f, 0.5f, info);
				entity->allocate_component<PhysicsComponent>()->handle = sphere;
				PhysicsSystem::set_handle_parent(sphere, entity);
			}
		}

		return true;
	}

	void render_frame(double frame_time, double) override
	{
		if (apply_anti_gravity)
		{
			auto &phys = scene.get_entity_pool().get_component_group<PhysicsComponent>();
			for (auto &p : phys)
			{
				Global::physics()->apply_force(get_component<PhysicsComponent>(p)->handle,
				                               vec3(0.0f, 300.0f, 0.0f));
			}
		}

		auto *node = PhysicsSystem::get_scene_node(camera_handle);
		node->transform.translation = camera.get_position();

		Global::physics()->iterate(frame_time);
		scene.update_cached_transforms();

#if 0
		{
			std::vector<PhysicsHandle *> ghost_collisions;
			Global::physics()->get_overlapping_objects(camera_handle, ghost_collisions);
			if (!ghost_collisions.empty())
				LOGI("Overlapping with %u objects!\n", unsigned(ghost_collisions.size()));
		}
#endif

		lighting.directional.direction = normalize(vec3(1.0f, 0.5f, 1.0f));
		lighting.directional.color = vec3(1.0f, 0.8f, 0.6f);
		renderer.set_mesh_renderer_options_from_lighting(lighting);
		context.set_camera(camera);
		visible.clear();
		scene.gather_visible_opaque_renderables(context.get_visibility_frustum(), visible);

		auto cmd = get_wsi().get_device().request_command_buffer();
		auto rp = get_wsi().get_device().get_swapchain_render_pass(Vulkan::SwapchainRenderPass::Depth);
		rp.clear_color[0].float32[0] = 0.01f;
		rp.clear_color[0].float32[1] = 0.02f;
		rp.clear_color[0].float32[2] = 0.03f;
		cmd->begin_render_pass(rp);

		renderer.begin();
		renderer.push_renderables(context, visible);
		renderer.flush(*cmd, context, 0);

		cmd->end_render_pass();

		get_wsi().get_device().submit(cmd);
	}

	Scene scene;
	AbstractRenderableHandle cube;
	AbstractRenderableHandle cone;
	AbstractRenderableHandle cylinder;
	AbstractRenderableHandle sphere;
	AbstractRenderableHandle plane;
	FPSCamera camera;
	RenderContext context;
	LightingParameters lighting;
	VisibilityList visible;
	Renderer renderer;
	std::string gltf_path;

	AbstractRenderableHandle gltf_mesh;
	unsigned gltf_mesh_physics_index = 0;
	PhysicsHandle *camera_handle = nullptr;

	bool apply_anti_gravity = false;
};

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	if (argc < 2)
		return nullptr;
	application_dummy();

	try
	{
		auto *app = new PhysicsSandboxApplication(argv[1]);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
}