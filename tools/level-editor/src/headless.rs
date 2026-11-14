//! Headless screenshot mode (`--shot out.png`): runs the same editor UI offscreen through egui + wgpu and writes
//! one PNG. No window is created, so it never appears on the desktop or takes focus.

use crate::app::{EditorApp, Gpu};
use crate::render::COLOR_FORMAT;
use egui_wgpu::wgpu;
use std::path::Path;

pub fn shot(app: &mut EditorApp, out: &Path, w: u32, h: u32, frames: usize) -> Result<(), String> {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: None,
        ..Default::default()
    }))
    .map_err(|e| format!("no GPU adapter: {e}"))?;
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
        .map_err(|e| format!("no GPU device: {e}"))?;
    let mut renderer = egui_wgpu::Renderer::new(&device, COLOR_FORMAT, egui_wgpu::RendererOptions::default());

    let target = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("shot"),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: COLOR_FORMAT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = target.create_view(&Default::default());
    let ctx = egui::Context::default();
    ctx.set_pixels_per_point(1.0);
    let sd = egui_wgpu::ScreenDescriptor { size_in_pixels: [w, h], pixels_per_point: 1.0 };

    // A few frames so panels settle their sizes and scroll positions before the captured one.
    for frame in 0..frames.max(2) {
        let raw = egui::RawInput {
            screen_rect: Some(egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(w as f32, h as f32))),
            time: Some(frame as f64 / 30.0),
            ..Default::default()
        };
        let out = ctx.run_ui(raw, |ui| {
            let mut gpu = Gpu { device: &device, queue: &queue, egui: &mut renderer };
            app.ui(ui, &mut gpu);
        });
        for (id, deltas) in &out.textures_delta.set {
            for delta in deltas {
                renderer.update_texture(&device, &queue, *id, delta);
            }
        }
        let jobs = ctx.tessellate(out.shapes, out.pixels_per_point);
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("egui") });
        let extra = renderer.update_buffers(&device, &queue, &mut enc, &jobs, &sd);
        {
            let pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("egui"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                })],
                ..Default::default()
            });
            renderer.render(&mut pass.forget_lifetime(), &jobs, &sd);
        }
        queue.submit(extra.into_iter().chain(std::iter::once(enc.finish())));
        for id in &out.textures_delta.free {
            renderer.free_texture(id);
        }
    }

    // Read back with 256-byte aligned rows, then drop the padding.
    let row = w * 4;
    let padded = row.div_ceil(256) * 256;
    let buf = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("readback"),
        size: (padded * h) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });
    let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("copy") });
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo { texture: &target, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::TexelCopyBufferInfo {
            buffer: &buf,
            layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(padded), rows_per_image: Some(h) },
        },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
    queue.submit([enc.finish()]);
    buf.map_async(wgpu::MapMode::Read, .., |r| {
        if let Err(e) = r {
            eprintln!("map failed: {e}");
        }
    });
    device.poll(wgpu::PollType::wait_indefinitely()).map_err(|e| format!("poll: {e}"))?;
    let data = buf.get_mapped_range(..).map_err(|e| format!("map: {e:?}"))?;
    let mut pixels = Vec::with_capacity((row * h) as usize);
    for y in 0..h {
        let s = (y * padded) as usize;
        pixels.extend_from_slice(&data[s..s + row as usize]);
    }
    drop(data);
    buf.unmap();

    if let Some(dir) = out.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let f = std::fs::File::create(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut wr = enc.write_header().map_err(|e| e.to_string())?;
    wr.write_image_data(&pixels).map_err(|e| e.to_string())?;
    Ok(())
}
