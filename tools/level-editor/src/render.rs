//! Offscreen wgpu scene renderer: course geometry (lit triangles), rail and spawn markers (unlit triangles),
//! other paths (lines). It draws into its own texture, which the UI shows as an egui image; the same code serves
//! the window and the headless `--shot` mode.

use crate::course::LoadedCourse;
use bytemuck::{Pod, Zeroable};
use egui_wgpu::wgpu;
use glam::{Mat4, Quat, Vec3};
use wgpu::util::DeviceExt;

pub const COLOR_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;
const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth32Float;
const MSAA: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct Vertex {
    pub pos: [f32; 3],
    pub nrm: [f32; 3],
    pub col: [f32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    light: [f32; 4],
    eye: [f32; 4],
}

#[derive(Clone, Copy, PartialEq)]
pub struct Layers {
    pub geometry: bool,
    pub rail: bool,
    pub paths: bool,
    pub spawns: bool,
    pub spawn_links: bool,
}

impl Default for Layers {
    fn default() -> Self {
        Self { geometry: true, rail: true, paths: true, spawns: true, spawn_links: true }
    }
}

#[derive(Default)]
pub struct CpuScene {
    pub tris: Vec<Vertex>,
    pub tri_idx: Vec<u32>,
    pub lines: Vec<Vertex>,
    pub overlay: Vec<Vertex>,
    pub overlay_idx: Vec<u32>,
    pub geo_min: [f32; 3],
    pub geo_max: [f32; 3],
    pub triangles: usize,
}

pub fn model_color(name: &str) -> [f32; 3] {
    let n = name.to_lowercase();
    let pick: Option<[f32; 3]> = [
        ("water", [0.25, 0.45, 0.65]),
        ("ground", [0.52, 0.47, 0.34]),
        ("grass", [0.36, 0.55, 0.25]),
        ("tree", [0.25, 0.45, 0.22]),
        ("hedge", [0.28, 0.48, 0.24]),
        ("tuta", [0.30, 0.50, 0.26]),
        ("ivy", [0.30, 0.50, 0.26]),
        ("flower", [0.75, 0.55, 0.65]),
        ("rock", [0.55, 0.53, 0.50]),
        ("stone", [0.60, 0.58, 0.54]),
        ("bridge", [0.55, 0.40, 0.25]),
        ("backdrop", [0.45, 0.52, 0.58]),
        ("sky", [0.55, 0.70, 0.90]),
    ]
    .iter()
    .find(|(k, _)| n.contains(k))
    .map(|x| x.1);
    pick.unwrap_or_else(|| {
        let h = crc32fast::hash(name.as_bytes());
        let f = |s: u32| 0.35 + ((h >> s) & 0xff) as f32 / 255.0 * 0.4;
        [f(0), f(8), f(16)]
    })
}

pub fn species_color(label: &str) -> [f32; 3] {
    let h = crc32fast::hash(label.as_bytes());
    // bright, saturated hues so markers stand out against the course
    let hue = (h % 360) as f32;
    hsv(hue, 0.85, 1.0)
}

fn hsv(h: f32, s: f32, v: f32) -> [f32; 3] {
    let c = v * s;
    let x = c * (1.0 - ((h / 60.0) % 2.0 - 1.0).abs());
    let m = v - c;
    let (r, g, b) = match (h / 60.0) as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    [r + m, g + m, b + m]
}

fn push_box(out_v: &mut Vec<Vertex>, out_i: &mut Vec<u32>, a: Vec3, b: Vec3, r: f32, col: [f32; 3]) {
    let d = (b - a).normalize_or_zero();
    if d == Vec3::ZERO {
        return;
    }
    let up = if d.y.abs() > 0.95 { Vec3::X } else { Vec3::Y };
    let s = d.cross(up).normalize() * r;
    let u = s.cross(d).normalize() * r;
    let corners = [s + u, -s + u, -s - u, s - u];
    let base = out_v.len() as u32;
    for p in [a, b] {
        for c in corners {
            out_v.push(Vertex { pos: (p + c).to_array(), nrm: [0.0; 3], col });
        }
    }
    for k in 0..4u32 {
        let k2 = (k + 1) % 4;
        out_i.extend_from_slice(&[base + k, base + k2, base + 4 + k, base + k2, base + 4 + k2, base + 4 + k]);
    }
}

fn push_marker(out_v: &mut Vec<Vertex>, out_i: &mut Vec<u32>, c: Vec3, r: f32, col: [f32; 3]) {
    // octahedron, flat shaded (one normal per face) so it reads as a solid gem
    let p = [
        c + Vec3::X * r,
        c - Vec3::X * r,
        c + Vec3::Y * r * 1.4,
        c - Vec3::Y * r * 1.4,
        c + Vec3::Z * r,
        c - Vec3::Z * r,
    ];
    let faces = [(0, 2, 4), (4, 2, 1), (1, 2, 5), (5, 2, 0), (4, 3, 0), (1, 3, 4), (5, 3, 1), (0, 3, 5)];
    for (a, b, d) in faces {
        let n = (p[b] - p[a]).cross(p[d] - p[a]).normalize();
        let base = out_v.len() as u32;
        for i in [a, b, d] {
            out_v.push(Vertex { pos: p[i].to_array(), nrm: n.to_array(), col });
        }
        out_i.extend_from_slice(&[base, base + 1, base + 2]);
    }
}

pub fn build_scene(c: &LoadedCourse, layers: Layers, selected_spawn: Option<(usize, usize)>) -> CpuScene {
    let mut s = CpuScene { geo_min: [f32::MAX; 3], geo_max: [f32::MIN; 3], ..Default::default() };
    if layers.geometry {
        for m in c.models.iter().filter(|m| m.visible) {
            let Some(mesh) = &m.mesh else { continue };
            let q = Quat::from_array(m.inst.rot);
            let q = if q.length_squared() < 1e-6 { Quat::IDENTITY } else { q.normalize() };
            let xf = Mat4::from_scale_rotation_translation(Vec3::from(m.inst.scale), q, Vec3::from(m.inst.pos));
            let nm = glam::Mat3::from_mat4(xf).inverse().transpose();
            let col = model_color(&m.inst.name);
            let base = s.tris.len() as u32;
            for (p, n) in mesh.positions.iter().zip(&mesh.normals) {
                let w = xf.transform_point3(Vec3::from(*p));
                for k in 0..3 {
                    s.geo_min[k] = s.geo_min[k].min(w[k]);
                    s.geo_max[k] = s.geo_max[k].max(w[k]);
                }
                let mut nn = (nm * Vec3::from(*n)).normalize_or_zero();
                if nn == Vec3::ZERO {
                    nn = Vec3::Y;
                }
                s.tris.push(Vertex { pos: w.to_array(), nrm: nn.to_array(), col });
            }
            s.tri_idx.extend(mesh.indices.iter().map(|i| base + i));
        }
        s.triangles = s.tri_idx.len() / 3;
    }
    let rail_entity = c.levels.iter().find_map(|l| l.rail);
    for l in &c.levels {
        for p in &l.paths {
            let is_rail = Some(p.entity) == rail_entity && l.rail.is_some();
            if is_rail {
                if !layers.rail {
                    continue;
                }
                for (si, seg) in p.segments.iter().enumerate() {
                    let col = if si == 0 { [1.0, 0.85, 0.1] } else { [1.0, 0.55, 0.1] };
                    for w in seg.windows(2) {
                        push_box(&mut s.overlay, &mut s.overlay_idx, Vec3::from(w[0]), Vec3::from(w[1]), 0.18, col);
                    }
                }
            } else if layers.paths {
                for seg in &p.segments {
                    for w in seg.windows(2) {
                        let col = [0.55, 0.85, 0.95];
                        s.lines.push(Vertex { pos: w[0], nrm: [0.0; 3], col });
                        s.lines.push(Vertex { pos: w[1], nrm: [0.0; 3], col });
                    }
                }
            }
        }
    }
    if layers.spawns {
        for (li, l) in c.levels.iter().enumerate() {
            for (si, sp) in l.spawns.iter().enumerate() {
                let sel = selected_spawn == Some((li, si));
                let col = if sel { [1.0, 1.0, 1.0] } else { species_color(&sp.label) };
                let pos = Vec3::from(sp.pos) + Vec3::Y * 0.6;
                push_marker(&mut s.overlay, &mut s.overlay_idx, pos, if sel { 0.9 } else { 0.5 }, col);
                // stem down to the placement point
                s.lines.push(Vertex { pos: sp.pos, nrm: [0.0; 3], col });
                s.lines.push(Vertex { pos: pos.to_array(), nrm: [0.0; 3], col });
                if layers.spawn_links || sel {
                    if let Some(path) = sp.path.and_then(|e| l.path(e)) {
                        for seg in &path.segments {
                            for w in seg.windows(2) {
                                s.lines.push(Vertex { pos: w[0], nrm: [0.0; 3], col });
                                s.lines.push(Vertex { pos: w[1], nrm: [0.0; 3], col });
                            }
                        }
                    }
                }
            }
        }
    }
    s
}

pub struct GpuScene {
    tri_vb: Option<wgpu::Buffer>,
    tri_ib: Option<wgpu::Buffer>,
    tri_n: u32,
    ov_vb: Option<wgpu::Buffer>,
    ov_ib: Option<wgpu::Buffer>,
    ov_n: u32,
    line_vb: Option<wgpu::Buffer>,
    line_n: u32,
}

impl GpuScene {
    pub fn upload(device: &wgpu::Device, s: &CpuScene) -> Self {
        let vb = |v: &[Vertex], l: &str| {
            (!v.is_empty()).then(|| {
                device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some(l),
                    contents: bytemuck::cast_slice(v),
                    usage: wgpu::BufferUsages::VERTEX,
                })
            })
        };
        let ib = |v: &[u32], l: &str| {
            (!v.is_empty()).then(|| {
                device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                    label: Some(l),
                    contents: bytemuck::cast_slice(v),
                    usage: wgpu::BufferUsages::INDEX,
                })
            })
        };
        Self {
            tri_vb: vb(&s.tris, "tri vb"),
            tri_ib: ib(&s.tri_idx, "tri ib"),
            tri_n: s.tri_idx.len() as u32,
            ov_vb: vb(&s.overlay, "overlay vb"),
            ov_ib: ib(&s.overlay_idx, "overlay ib"),
            ov_n: s.overlay_idx.len() as u32,
            line_vb: vb(&s.lines, "line vb"),
            line_n: s.lines.len() as u32,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct OrbitCamera {
    pub target: Vec3,
    pub yaw: f32,
    pub pitch: f32,
    pub dist: f32,
}

impl OrbitCamera {
    pub fn frame(min: Vec3, max: Vec3) -> Self {
        let c = (min + max) * 0.5;
        let ext = (max - min).length().max(1.0);
        Self { target: c, yaw: 0.6, pitch: 0.85, dist: ext * 0.85 }
    }
    pub fn eye(&self) -> Vec3 {
        let d = Vec3::new(self.pitch.cos() * self.yaw.sin(), self.pitch.sin(), self.pitch.cos() * self.yaw.cos());
        self.target + d * self.dist
    }
    pub fn view_proj(&self, aspect: f32) -> Mat4 {
        use glam::camera::rh::{proj::directx, view};
        let proj = directx::perspective(50f32.to_radians(), aspect.max(0.1), (self.dist * 0.002).max(0.05), 20000.0);
        proj * view::look_at_mat4(self.eye(), self.target, Vec3::Y)
    }
    pub fn orbit(&mut self, dx: f32, dy: f32) {
        self.yaw -= dx * 0.008;
        self.pitch = (self.pitch + dy * 0.008).clamp(-1.5, 1.5);
    }
    pub fn pan(&mut self, dx: f32, dy: f32) {
        let eye = self.eye();
        let fwd = (self.target - eye).normalize();
        let right = fwd.cross(Vec3::Y).normalize();
        let up = right.cross(fwd);
        let k = self.dist * 0.0015;
        self.target += (-right * dx + up * dy) * k;
    }
    pub fn zoom(&mut self, steps: f32) {
        self.dist = (self.dist * (1.0 - steps * 0.1)).clamp(0.5, 10000.0);
    }
}

pub struct Viewport {
    tri_pipe: wgpu::RenderPipeline,
    line_pipe: wgpu::RenderPipeline,
    ubuf: wgpu::Buffer,
    bind: wgpu::BindGroup,
    pub size: [u32; 2],
    msaa_tex: Option<wgpu::TextureView>,
    depth: Option<wgpu::TextureView>,
    pub color: Option<wgpu::Texture>,
    pub color_view: Option<wgpu::TextureView>,
}

const SHADER: &str = r#"
struct U { view_proj: mat4x4<f32>, light: vec4<f32>, eye: vec4<f32> };
@group(0) @binding(0) var<uniform> u: U;
struct VI { @location(0) pos: vec3<f32>, @location(1) nrm: vec3<f32>, @location(2) col: vec3<f32> };
struct VO { @builtin(position) clip: vec4<f32>, @location(0) nrm: vec3<f32>, @location(1) col: vec3<f32>, @location(2) wpos: vec3<f32> };
@vertex fn vs(v: VI) -> VO {
  var o: VO;
  o.clip = u.view_proj * vec4<f32>(v.pos, 1.0);
  o.nrm = v.nrm; o.col = v.col; o.wpos = v.pos;
  return o;
}
@fragment fn fs(i: VO) -> @location(0) vec4<f32> {
  let l = length(i.nrm);
  if (l < 0.01) { return vec4<f32>(i.col, 1.0); }
  let n = i.nrm / l;
  let d = abs(dot(n, normalize(u.light.xyz)));
  let hemi = 0.5 + 0.5 * abs(n.y);
  var c = i.col * (0.30 + 0.55 * d + 0.25 * hemi);
  let fog = clamp(length(i.wpos - u.eye.xyz) / u.eye.w, 0.0, 1.0);
  c = mix(c, vec3<f32>(0.62, 0.70, 0.78), fog * fog * 0.5);
  return vec4<f32>(c, 1.0);
}
"#;

impl Viewport {
    pub fn new(device: &wgpu::Device) -> Self {
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("scene"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });
        let ubuf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("uniforms"),
            size: std::mem::size_of::<Uniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("bg"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: ubuf.as_entire_binding() }],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pl"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        let attrs = wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Float32x3];
        let make = |topo: wgpu::PrimitiveTopology, bias: i32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("scene pipe"),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &module,
                    entry_point: Some("vs"),
                    compilation_options: Default::default(),
                    buffers: &[Some(wgpu::VertexBufferLayout {
                        array_stride: std::mem::size_of::<Vertex>() as u64,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &attrs,
                    })],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fs"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: COLOR_FORMAT,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState { topology: topo, cull_mode: None, ..Default::default() },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FORMAT,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(wgpu::CompareFunction::Less),
                    stencil: Default::default(),
                    bias: wgpu::DepthBiasState { constant: bias, slope_scale: 0.0, clamp: 0.0 },
                }),
                multisample: wgpu::MultisampleState { count: MSAA, ..Default::default() },
                multiview_mask: None,
                cache: None,
            })
        };
        Self {
            tri_pipe: make(wgpu::PrimitiveTopology::TriangleList, 0),
            line_pipe: make(wgpu::PrimitiveTopology::LineList, 0),
            ubuf,
            bind,
            size: [0, 0],
            msaa_tex: None,
            depth: None,
            color: None,
            color_view: None,
        }
    }

    /// Returns true when the colour texture was recreated (the UI must re-register it).
    pub fn resize(&mut self, device: &wgpu::Device, w: u32, h: u32) -> bool {
        let (w, h) = (w.clamp(16, 8192), h.clamp(16, 8192));
        if self.size == [w, h] && self.color.is_some() {
            return false;
        }
        self.size = [w, h];
        let ext = wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 };
        let tex = |label, format, samples, usage| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: ext,
                mip_level_count: 1,
                sample_count: samples,
                dimension: wgpu::TextureDimension::D2,
                format,
                usage,
                view_formats: &[],
            })
        };
        let color = tex(
            "viewport color",
            COLOR_FORMAT,
            1,
            wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_SRC,
        );
        self.msaa_tex =
            Some(tex("msaa", COLOR_FORMAT, MSAA, wgpu::TextureUsages::RENDER_ATTACHMENT).create_view(&Default::default()));
        self.depth =
            Some(tex("depth", DEPTH_FORMAT, MSAA, wgpu::TextureUsages::RENDER_ATTACHMENT).create_view(&Default::default()));
        self.color_view = Some(color.create_view(&Default::default()));
        self.color = Some(color);
        true
    }

    pub fn render(&self, device: &wgpu::Device, queue: &wgpu::Queue, scene: Option<&GpuScene>, cam: &OrbitCamera, fog_dist: f32) {
        let (Some(msaa), Some(depth), Some(color)) = (&self.msaa_tex, &self.depth, &self.color_view) else { return };
        let aspect = self.size[0] as f32 / self.size[1] as f32;
        let eye = cam.eye();
        let u = Uniforms {
            view_proj: cam.view_proj(aspect).to_cols_array_2d(),
            light: [0.35, 0.85, 0.4, 0.0],
            eye: [eye.x, eye.y, eye.z, fog_dist.max(1.0)],
        };
        queue.write_buffer(&self.ubuf, 0, bytemuck::bytes_of(&u));
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("viewport") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("viewport pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: msaa,
                    resolve_target: Some(color),
                    depth_slice: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.36, g: 0.45, b: 0.55, a: 1.0 }),
                        store: wgpu::StoreOp::Discard,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Discard }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            rp.set_bind_group(0, &self.bind, &[]);
            if let Some(s) = scene {
                if let (Some(vb), Some(ib)) = (&s.tri_vb, &s.tri_ib) {
                    rp.set_pipeline(&self.tri_pipe);
                    rp.set_vertex_buffer(0, vb.slice(..));
                    rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
                    rp.draw_indexed(0..s.tri_n, 0, 0..1);
                }
                if let Some(vb) = &s.line_vb {
                    rp.set_pipeline(&self.line_pipe);
                    rp.set_vertex_buffer(0, vb.slice(..));
                    rp.draw(0..s.line_n, 0..1);
                }
                if let (Some(vb), Some(ib)) = (&s.ov_vb, &s.ov_ib) {
                    rp.set_pipeline(&self.tri_pipe);
                    rp.set_vertex_buffer(0, vb.slice(..));
                    rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint32);
                    rp.draw_indexed(0..s.ov_n, 0, 0..1);
                }
            }
        }
        queue.submit([enc.finish()]);
    }
}
