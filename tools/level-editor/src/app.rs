//! Editor UI (egui). Read only in milestone 1: pick a course pack and leg, list its parts, show geometry, the
//! NEO-ONE rail, other paths and spawn points in the 3D view.

use crate::course::{CourseEntry, DataRoot, LoadedCourse, Variant};
use crate::level::kind_name;
use crate::render::{self, GpuScene, Layers, OrbitCamera, Viewport};
use egui_wgpu::wgpu;
use glam::Vec3;

pub struct Gpu<'a> {
    pub device: &'a wgpu::Device,
    pub queue: &'a wgpu::Queue,
    pub egui: &'a mut egui_wgpu::Renderer,
}

pub struct EditorApp {
    data: Result<DataRoot, String>,
    courses: Vec<CourseEntry>,
    filter: String,
    selected_course: Option<usize>,
    variants: Vec<Variant>,
    loaded: Option<LoadedCourse>,
    load_error: Option<String>,
    layers: Layers,
    cam: OrbitCamera,
    viewport: Option<Viewport>,
    tex_id: Option<egui::TextureId>,
    gpu_scene: Option<GpuScene>,
    scene_dirty: bool,
    scene_tris: usize,
    fog_dist: f32,
    selected_spawn: Option<(usize, usize)>,
    pub expand_all: bool,
}

impl EditorApp {
    pub fn new(data: Result<DataRoot, String>) -> Self {
        let courses = data.as_ref().map(|d| d.courses()).unwrap_or_default();
        Self {
            data,
            courses,
            filter: String::new(),
            selected_course: None,
            variants: vec![],
            loaded: None,
            load_error: None,
            layers: Layers::default(),
            cam: OrbitCamera { target: Vec3::ZERO, yaw: 0.6, pitch: 0.8, dist: 100.0 },
            viewport: None,
            tex_id: None,
            gpu_scene: None,
            scene_dirty: true,
            scene_tris: 0,
            fog_dist: 400.0,
            selected_spawn: None,
            expand_all: false,
        }
    }

    pub fn open(&mut self, course: &str, variant: u32) -> Result<(), String> {
        let idx = self
            .courses
            .iter()
            .position(|c| c.name.eq_ignore_ascii_case(course))
            .ok_or_else(|| format!("course pack {course} not found"))?;
        self.select_course(idx);
        self.load_variant(variant);
        match &self.load_error {
            Some(e) => Err(e.clone()),
            None => Ok(()),
        }
    }

    fn select_course(&mut self, idx: usize) {
        self.selected_course = Some(idx);
        self.variants = match &self.data {
            Ok(d) => d.variants(&self.courses[idx]).unwrap_or_default(),
            Err(_) => vec![],
        };
    }

    fn load_variant(&mut self, no: u32) {
        let Some(idx) = self.selected_course else { return };
        let course = self.courses[idx].clone();
        let Ok(data) = &mut self.data else { return };
        match data.load(&course, no) {
            Ok(c) => {
                self.load_error = None;
                self.frame_course(&c);
                self.loaded = Some(c);
                self.selected_spawn = None;
                self.scene_dirty = true;
            }
            Err(e) => self.load_error = Some(e),
        }
    }

    fn frame_course(&mut self, c: &LoadedCourse) {
        // Frame the rail plus spawns (the playable area); geometry includes huge backdrops.
        let mut pts: Vec<Vec3> = vec![];
        if let Some(r) = c.rail() {
            pts.extend(r.segments.iter().flatten().map(|p| Vec3::from(*p)));
        }
        for l in &c.levels {
            pts.extend(l.spawns.iter().map(|s| Vec3::from(s.pos)));
        }
        if pts.is_empty() {
            pts.push(Vec3::ZERO);
        }
        let min = pts.iter().fold(Vec3::splat(f32::MAX), |a, b| a.min(*b));
        let max = pts.iter().fold(Vec3::splat(f32::MIN), |a, b| a.max(*b));
        self.cam = OrbitCamera::frame(min, max);
        self.fog_dist = (max - min).length() * 3.0;
    }

    fn focus(&mut self, p: [f32; 3]) {
        self.cam.target = Vec3::from(p);
        self.cam.dist = 18.0;
    }

    pub fn ui(&mut self, ui: &mut egui::Ui, gpu: &mut Gpu) {
        egui::Panel::top("top").show(ui, |ui| self.top_bar(ui));
        egui::Panel::left("courses").default_size(190.0).show(ui, |ui| self.course_list(ui));
        egui::Panel::right("parts").default_size(380.0).show(ui, |ui| self.parts(ui));
        egui::CentralPanel::default().show(ui, |ui| self.viewport_ui(ui, gpu));
    }

    fn top_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading("Snap level editor");
            ui.label("milestone 1: read-only course viewer");
            ui.separator();
            match &self.data {
                Ok(d) => ui.label(format!("data: {}", d.root.display())),
                Err(e) => ui.colored_label(egui::Color32::LIGHT_RED, e),
            };
        });
        if let Some(idx) = self.selected_course {
            ui.horizontal(|ui| {
                ui.label(format!("course pack {}", self.courses[idx].name));
                let cur = self.loaded.as_ref().map(|c| c.variant);
                let nos: Vec<u32> = self.variants.iter().map(|v| v.no).collect();
                for no in nos {
                    if ui.selectable_label(cur == Some(no), format!("leg {no}")).clicked() {
                        self.load_variant(no);
                    }
                }
                if let Some(c) = &self.loaded {
                    ui.label(format!("loaded in {} ms", c.load_ms));
                }
                if let Some(e) = &self.load_error {
                    ui.colored_label(egui::Color32::LIGHT_RED, e);
                }
            });
        }
    }

    fn course_list(&mut self, ui: &mut egui::Ui) {
        ui.label(format!("{} course packs", self.courses.len()));
        ui.text_edit_singleline(&mut self.filter);
        let f = self.filter.to_lowercase();
        let mut clicked = None;
        egui::ScrollArea::vertical().show(ui, |ui| {
            for (i, c) in self.courses.iter().enumerate() {
                if !f.is_empty() && !c.name.to_lowercase().contains(&f) {
                    continue;
                }
                let sel = self.selected_course == Some(i);
                let r = ui.selectable_label(sel, &c.name);
                if sel && self.expand_all {
                    r.scroll_to_me(Some(egui::Align::Center));
                }
                if r.clicked() {
                    clicked = Some(i);
                }
            }
        });
        if let Some(i) = clicked {
            self.select_course(i);
            let first = self.variants.first().map(|v| v.no).unwrap_or(1);
            self.load_variant(first);
        }
    }

    fn parts(&mut self, ui: &mut egui::Ui) {
        let Some(c) = &mut self.loaded else {
            ui.label("Pick a course pack on the left.");
            return;
        };
        let open = self.expand_all;
        let mut focus = None;
        let mut new_sel = None;
        let mut dirty = false;
        egui::ScrollArea::vertical().show(ui, |ui| {
            ui.heading(format!("{} leg {}", c.course, c.variant));
            let rail_len: Vec<f32> = c
                .rail()
                .map(|r| (0..r.segments.len()).map(|i| r.length(i)).collect())
                .unwrap_or_default();
            egui::CollapsingHeader::new(format!("Levels ({})", c.levels.len())).default_open(true).show(ui, |ui| {
                if let Some(v) = c.variants.iter().find(|v| v.no == c.variant) {
                    for lr in &v.levels {
                        match &lr.name {
                            Some(n) => ui.label(n),
                            None => ui.weak(format!("hash {:08x}: no entry with that name in the pack", lr.hash)),
                        };
                    }
                }
                for e in &c.level_errors {
                    ui.colored_label(egui::Color32::LIGHT_RED, e);
                }
            });
            for l in &c.levels {
                egui::CollapsingHeader::new(format!("{}  ({})", l.name, l.kind())).default_open(false).show(ui, |ui| {
                    ui.label(format!("{} bytes, {} entities, {} scene nodes", l.size, l.entity_count, l.node_count));
                    for (k, n) in &l.component_kinds {
                        ui.label(format!("  component {k}: {n}"));
                    }
                    if !l.species.is_empty() {
                        ui.label(format!("species used: {:?}", l.species));
                    }
                    if !l.env_params.is_empty() {
                        ui.label(format!("environment parameters: {}", l.env_params.len()));
                    }
                });
            }
            egui::CollapsingHeader::new("Camera rail").default_open(true).show(ui, |ui| {
                if rail_len.is_empty() {
                    ui.label("no rail found (course parameters s6 -> node -> path)");
                }
                for (i, len) in rail_len.iter().enumerate() {
                    let pts = c.rail().map(|r| r.segments[i].len()).unwrap_or(0);
                    ui.label(format!("segment {i}: {pts} points, {len:.1} units"));
                }
            });
            let total_paths: usize = c.levels.iter().map(|l| l.paths.len()).sum();
            ui.label(format!("Other paths: {}", total_paths.saturating_sub(rail_len.is_empty() as usize ^ 1)));
            let total_spawns: usize = c.levels.iter().map(|l| l.spawns.len()).sum();
            egui::CollapsingHeader::new(format!("Spawns ({total_spawns})")).default_open(open).show(ui, |ui| {
                ui.weak("click to select, double-click to focus the camera");
                for (li, l) in c.levels.iter().enumerate() {
                    for (si, s) in l.spawns.iter().enumerate() {
                        let col = render::species_color(&s.label);
                        let col = egui::Color32::from_rgb((col[0] * 255.0) as u8, (col[1] * 255.0) as u8, (col[2] * 255.0) as u8);
                        let txt = format!(
                            "{}  ({:.1}, {:.1}, {:.1}){}",
                            s.label,
                            s.pos[0],
                            s.pos[1],
                            s.pos[2],
                            if s.pos_from_path { "  [path start]" } else { "" }
                        );
                        let sel = self.selected_spawn == Some((li, si));
                        let r = ui.selectable_label(sel, egui::RichText::new(txt).color(col));
                        if r.clicked() {
                            new_sel = Some((li, si));
                        }
                        if r.double_clicked() {
                            focus = Some(s.pos);
                        }
                    }
                }
            });
            let total_models = c.models.len();
            let missing = c.models.iter().filter(|m| m.mesh.is_none()).count();
            egui::CollapsingHeader::new(format!("Model instances ({total_models}, {missing} unresolved)"))
                .default_open(open)
                .show(ui, |ui| {
                    for m in &mut c.models {
                        ui.horizontal(|ui| {
                            if ui.checkbox(&mut m.visible, "").changed() {
                                dirty = true;
                            }
                            let tris = m.mesh.as_ref().map(|x| x.indices.len() / 3).unwrap_or(0);
                            let t = format!("{}  {} tris", m.inst.name, tris);
                            match &m.error {
                                Some(e) => ui.colored_label(egui::Color32::LIGHT_RED, format!("{t}: {e}")),
                                None => ui.label(t),
                            };
                        });
                    }
                });
            egui::CollapsingHeader::new("Component kinds (all levels)").default_open(false).show(ui, |ui| {
                ui.weak("slot number of the component sub-table; names are working labels");
                for s in [1usize, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15] {
                    ui.label(format!("{s:>2}: {}", kind_name(s)));
                }
            });
        });
        if dirty {
            self.scene_dirty = true;
        }
        if let Some(s) = new_sel {
            self.selected_spawn = if self.selected_spawn == Some(s) { None } else { Some(s) };
            self.scene_dirty = true;
        }
        if let Some(p) = focus {
            self.focus(p);
        }
    }

    fn viewport_ui(&mut self, ui: &mut egui::Ui, gpu: &mut Gpu) {
        ui.horizontal(|ui| {
            let before = self.layers;
            ui.checkbox(&mut self.layers.geometry, "geometry");
            ui.checkbox(&mut self.layers.rail, "rail");
            ui.checkbox(&mut self.layers.paths, "other paths");
            ui.checkbox(&mut self.layers.spawns, "spawns");
            ui.checkbox(&mut self.layers.spawn_links, "spawn paths");
            if before != self.layers {
                self.scene_dirty = true;
            }
            if ui.button("frame course").clicked() {
                if let Some(c) = self.loaded.take() {
                    self.frame_course(&c);
                    self.loaded = Some(c);
                }
            }
            ui.weak("drag: orbit   right/middle drag: pan   wheel: zoom");
        });
        let avail = ui.available_size();
        let ppp = ui.ctx().pixels_per_point();
        let (w, h) = ((avail.x * ppp).max(16.0) as u32, (avail.y * ppp).max(16.0) as u32);
        let vp = self.viewport.get_or_insert_with(|| Viewport::new(gpu.device));
        if vp.resize(gpu.device, w, h) {
            let view = vp.color_view.as_ref().unwrap();
            match self.tex_id {
                Some(id) => gpu.egui.update_egui_texture_from_wgpu_texture(gpu.device, view, wgpu::FilterMode::Linear, id),
                None => self.tex_id = Some(gpu.egui.register_native_texture(gpu.device, view, wgpu::FilterMode::Linear)),
            }
        }
        if self.scene_dirty {
            if let Some(c) = &self.loaded {
                let s = render::build_scene(c, self.layers, self.selected_spawn);
                self.scene_tris = s.triangles;
                self.gpu_scene = Some(GpuScene::upload(gpu.device, &s));
            }
            self.scene_dirty = false;
        }
        vp.render(gpu.device, gpu.queue, self.gpu_scene.as_ref(), &self.cam, self.fog_dist);
        let Some(id) = self.tex_id else { return };
        let resp = ui.add(egui::Image::new((id, avail)).sense(egui::Sense::click_and_drag()));
        let d = resp.drag_delta();
        if resp.dragged_by(egui::PointerButton::Primary) {
            self.cam.orbit(d.x, d.y);
        } else if resp.dragged_by(egui::PointerButton::Secondary) || resp.dragged_by(egui::PointerButton::Middle) {
            self.cam.pan(d.x, d.y);
        }
        if resp.hovered() {
            let scroll = ui.input(|i| i.smooth_scroll_delta.y);
            if scroll != 0.0 {
                self.cam.zoom(scroll / 50.0);
            }
        }
        let info = match &self.loaded {
            Some(c) => format!(
                "{} leg {}   {} triangles   rail: {}   spawns: {}",
                c.course,
                c.variant,
                self.scene_tris,
                c.rail().map(|r| format!("{:.1} units", r.length(0))).unwrap_or("none".into()),
                c.levels.iter().map(|l| l.spawns.len()).sum::<usize>()
            ),
            None => "no course loaded".into(),
        };
        ui.painter().text(
            resp.rect.left_top() + egui::vec2(8.0, 8.0),
            egui::Align2::LEFT_TOP,
            info,
            egui::FontId::proportional(14.0),
            egui::Color32::WHITE,
        );
    }
}
