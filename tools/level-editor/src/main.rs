//! New Pokémon Snap recomp level editor, milestone 1: a read-only course viewer.
//!
//! level-editor [--data <extracted/drp-v262144>] [--course LC_001_A] [--variant 1]
//!              [--shot out.png [--size 1600x900]] [--summary]
//!
//! Without --shot it opens a window. With --shot it renders the same UI offscreen and writes a PNG (no window).
//! --summary prints the course parts as text. Nothing is ever written to the data folder.

mod app;
mod course;
mod fb;
mod geom;
mod headless;
mod level;
mod render;

use std::path::PathBuf;

struct Args {
    data: Option<PathBuf>,
    course: Option<String>,
    variant: u32,
    shot: Option<PathBuf>,
    size: (u32, u32),
    summary: bool,
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args { data: None, course: None, variant: 1, shot: None, size: (1600, 900), summary: false };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let mut val = || it.next().ok_or(format!("{k} needs a value"));
        match k.as_str() {
            "--data" => a.data = Some(val()?.into()),
            "--course" => a.course = Some(val()?),
            "--variant" => a.variant = val()?.parse().map_err(|_| "--variant: number expected")?,
            "--shot" => a.shot = Some(val()?.into()),
            "--size" => {
                let v = val()?;
                let (w, h) = v.split_once('x').ok_or("--size WxH")?;
                a.size = (w.parse().map_err(|_| "--size WxH")?, h.parse().map_err(|_| "--size WxH")?);
            }
            "--summary" => a.summary = true,
            "-h" | "--help" => {
                println!("level-editor [--data DIR] [--course LC_001_A] [--variant 1] [--shot out.png] [--size WxH] [--summary]");
                std::process::exit(0);
            }
            _ => return Err(format!("unknown argument {k}")),
        }
    }
    Ok(a)
}

fn print_summary(data: &mut course::DataRoot, name: &str, variant: u32) -> Result<(), String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    println!("{} leg {} ({} legs, loaded in {} ms)", lc.course, lc.variant, lc.variants.len(), lc.load_ms);
    if let Some(v) = lc.variants.iter().find(|v| v.no == lc.variant) {
        for r in &v.levels {
            println!("  level ref {:08x} -> {}", r.hash, r.name.as_deref().unwrap_or("(unresolved)"));
        }
    }
    for l in &lc.levels {
        println!(
            "{} [{}]: {} bytes, {} entities, {} nodes, {} models, {} paths, {} spawns, {} templates",
            l.name,
            l.kind(),
            l.size,
            l.entity_count,
            l.node_count,
            l.models.len(),
            l.paths.len(),
            l.spawns.len(),
            l.templates.len()
        );
        for (k, n) in &l.component_kinds {
            println!("    component {k}: {n}");
        }
        if !l.species.is_empty() {
            println!("    species: {:?}", l.species);
        }
    }
    for e in &lc.level_errors {
        println!("  error: {e}");
    }
    match lc.rail() {
        Some(r) => {
            for i in 0..r.segments.len() {
                println!("rail segment {i}: {} points, {:.1} units", r.segments[i].len(), r.length(i));
            }
        }
        None => println!("rail: not found"),
    }
    let resolved = lc.models.iter().filter(|m| m.mesh.is_some()).count();
    let tris: usize = lc.models.iter().filter_map(|m| m.mesh.as_ref()).map(|m| m.indices.len() / 3).sum();
    println!("model instances: {} ({} resolved, {} triangles)", lc.models.len(), resolved, tris);
    for m in lc.models.iter().filter(|m| m.error.is_some()) {
        println!("  unresolved {}: {}", m.inst.name, m.error.as_deref().unwrap_or(""));
    }
    let mut by_species: std::collections::BTreeMap<&str, usize> = Default::default();
    for l in &lc.levels {
        for s in &l.spawns {
            *by_species.entry(s.label.as_str()).or_default() += 1;
        }
    }
    println!("spawns by species: {by_species:?}");
    Ok(())
}

fn main() {
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    };
    let data = course::DataRoot::locate(args.data.as_deref());
    if args.summary {
        let r = match data {
            Ok(mut d) => print_summary(&mut d, args.course.as_deref().unwrap_or("LC_001_A"), args.variant),
            Err(e) => Err(e),
        };
        if let Err(e) = r {
            eprintln!("{e}");
            std::process::exit(1);
        }
        return;
    }
    let mut ed = app::EditorApp::new(data);
    if let Some(c) = &args.course {
        if let Err(e) = ed.open(c, args.variant) {
            eprintln!("{e}");
        }
    }
    if let Some(out) = &args.shot {
        ed.expand_all = true;
        match headless::shot(&mut ed, out, args.size.0, args.size.1, 4) {
            Ok(()) => println!("wrote {}", out.display()),
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }
        return;
    }
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([1600.0, 900.0]).with_title("Snap level editor"),
        renderer: eframe::Renderer::Wgpu,
        ..Default::default()
    };
    let r = eframe::run_native("Snap level editor", opts, Box::new(|_cc| Ok(Box::new(Window(ed)))));
    if let Err(e) = r {
        eprintln!("{e}");
    }
}

struct Window(app::EditorApp);

impl eframe::App for Window {
    fn ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame) {
        let Some(rs) = frame.wgpu_render_state() else {
            ui.label("wgpu renderer not available");
            return;
        };
        let mut r = rs.renderer.write();
        let mut gpu = app::Gpu { device: &rs.device, queue: &rs.queue, egui: &mut r };
        self.0.ui(ui, &mut gpu);
    }
}
