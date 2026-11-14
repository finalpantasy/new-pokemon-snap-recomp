//! Course packs from the local unpacked data (`extracted/drp-v262144/nx/Pack/LevelComposite/master/LC_*.drp/`).
//! Read only: nothing here writes to the data folder.
//!
//! A course pack holds one `courseb` whose variants (the legs of a route, e.g. Florio 001_1 and 001_2) list CRC32
//! hashes of level entry names (`LV_ENV_*`, `LV_MAP_*`, `LV_SND_*`, ...). Hash = zlib CRC32 of the name without
//! extension. Model instances in the levels name SSBH models found as `<name>_LightmapUV.nusrcmdlb` or
//! `<name>.nusrcmdlb` in the pack (fallback: anywhere under `nx/Pack`).

use crate::fb::Buf;
use crate::geom::{self, ModelMesh};
use crate::level::{self, Level};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub const PACK_SUBDIR: &str = "nx/Pack/LevelComposite/master";

pub struct DataRoot {
    pub root: PathBuf,
    global_models: Option<HashMap<String, PathBuf>>,
}

#[derive(Clone)]
pub struct CourseEntry {
    pub name: String,
    pub dir: PathBuf,
}

#[derive(Clone, Debug)]
pub struct LevelRef {
    pub hash: u32,
    pub name: Option<String>,
    pub file: Option<PathBuf>,
}

#[derive(Clone, Debug)]
pub struct Variant {
    pub no: u32,
    pub levels: Vec<LevelRef>,
}

pub struct PlacedModel {
    pub inst: level::ModelInstance,
    pub level: String,
    pub source: Option<PathBuf>,
    pub mesh: Option<std::sync::Arc<ModelMesh>>,
    pub error: Option<String>,
    pub visible: bool,
}

pub struct LoadedCourse {
    pub course: String,
    pub variant: u32,
    pub variants: Vec<Variant>,
    pub levels: Vec<Level>,
    pub level_errors: Vec<String>,
    pub models: Vec<PlacedModel>,
    pub load_ms: u128,
}

impl DataRoot {
    /// Finds `extracted/drp-v262144` from an explicit path, the current directory or the exe location, walking up.
    pub fn locate(explicit: Option<&Path>) -> Result<Self, String> {
        let ok = |p: &Path| p.join(PACK_SUBDIR).is_dir();
        if let Some(p) = explicit {
            return if ok(p) {
                Ok(Self { root: p.to_path_buf(), global_models: None })
            } else {
                Err(format!("{} has no {PACK_SUBDIR}", p.display()))
            };
        }
        let mut starts = vec![];
        if let Ok(c) = std::env::current_dir() {
            starts.push(c);
        }
        if let Ok(e) = std::env::current_exe() {
            starts.push(e);
        }
        for s in starts {
            for a in s.ancestors() {
                let cand = a.join("extracted").join("drp-v262144");
                if ok(&cand) {
                    return Ok(Self { root: cand, global_models: None });
                }
            }
        }
        Err("unpacked data not found: pass --data <path to extracted/drp-v262144>".into())
    }

    pub fn courses(&self) -> Vec<CourseEntry> {
        let mut v: Vec<CourseEntry> = std::fs::read_dir(self.root.join(PACK_SUBDIR))
            .map(|rd| {
                rd.filter_map(|e| e.ok())
                    .filter(|e| e.path().is_dir())
                    .filter_map(|e| {
                        let n = e.file_name().to_string_lossy().to_string();
                        let name = n.strip_suffix(".drp")?.to_string();
                        name.starts_with("LC_").then(|| CourseEntry { name, dir: e.path() })
                    })
                    .collect()
            })
            .unwrap_or_default();
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v
    }

    fn global_model(&mut self, file_name: &str) -> Option<PathBuf> {
        if self.global_models.is_none() {
            let mut m = HashMap::new();
            for e in walkdir::WalkDir::new(self.root.join("nx/Pack")).into_iter().filter_map(|e| e.ok()) {
                let n = e.file_name().to_string_lossy();
                if n.ends_with(".nusrcmdlb") {
                    m.entry(n.to_lowercase()).or_insert_with(|| e.path().to_path_buf());
                }
            }
            self.global_models = Some(m);
        }
        self.global_models.as_ref()?.get(&file_name.to_lowercase()).cloned()
    }

    pub fn variants(&self, c: &CourseEntry) -> Result<Vec<Variant>, String> {
        let cb = c.dir.join(format!("{}.courseb", c.name));
        let bytes = std::fs::read(&cb).map_err(|e| format!("{}: {e}", cb.display()))?;
        let root = Buf(&bytes).root().ok_or("courseb: bad root")?;
        // name hash -> (stem, path) for every file in the pack
        let mut by_hash: HashMap<u32, (String, PathBuf)> = HashMap::new();
        if let Ok(rd) = std::fs::read_dir(&c.dir) {
            for e in rd.filter_map(|e| e.ok()) {
                let fname = e.file_name().to_string_lossy().to_string();
                let stem = fname.split('.').next().unwrap_or("").to_string();
                let h = crc32fast::hash(stem.as_bytes());
                let is_level = fname.ends_with(".levelb");
                let replace = match by_hash.get(&h) {
                    None => true,
                    Some((_, p)) => is_level && !p.to_string_lossy().ends_with(".levelb"),
                };
                if replace {
                    by_hash.insert(h, (stem, e.path()));
                }
            }
        }
        Ok(root
            .tables(1)
            .iter()
            .map(|v| Variant {
                no: v.u32(0).unwrap_or(0),
                levels: v
                    .u32s(1)
                    .into_iter()
                    .map(|h| {
                        let hit = by_hash.get(&h);
                        LevelRef { hash: h, name: hit.map(|x| x.0.clone()), file: hit.map(|x| x.1.clone()) }
                    })
                    .collect(),
            })
            .collect())
    }

    pub fn load(&mut self, c: &CourseEntry, variant_no: u32) -> Result<LoadedCourse, String> {
        let t0 = std::time::Instant::now();
        let variants = self.variants(c)?;
        let v = variants
            .iter()
            .find(|v| v.no == variant_no)
            .or(variants.first())
            .ok_or("courseb has no variants")?
            .clone();
        let mut levels = vec![];
        let mut level_errors = vec![];
        for lr in &v.levels {
            let Some(f) = &lr.file else { continue };
            if f.extension().and_then(|e| e.to_str()) != Some("levelb") {
                continue;
            }
            match std::fs::read(f).map_err(|e| e.to_string()).and_then(|b| level::parse(&b, &f.display().to_string())) {
                Ok(l) => levels.push(l),
                Err(e) => level_errors.push(format!("{}: {e}", f.display())),
            }
        }
        let mut cache: HashMap<PathBuf, Result<std::sync::Arc<ModelMesh>, String>> = HashMap::new();
        let mut models = vec![];
        for l in &levels {
            for inst in &l.models {
                let cands = [format!("{}_LightmapUV.nusrcmdlb", inst.name), format!("{}.nusrcmdlb", inst.name)];
                let mut src = cands.iter().map(|n| c.dir.join(n)).find(|p| p.is_file());
                if src.is_none() {
                    src = cands.iter().find_map(|n| self.global_model(n));
                }
                let (mesh, error) = match &src {
                    Some(p) => match cache
                        .entry(p.clone())
                        .or_insert_with(|| geom::load_model(p).map(std::sync::Arc::new))
                    {
                        Ok(m) => (Some(m.clone()), None),
                        Err(e) => (None, Some(e.clone())),
                    },
                    None => (None, Some("model file not found".to_string())),
                };
                let visible = default_visible(&inst.name);
                models.push(PlacedModel { inst: inst.clone(), level: l.name.clone(), source: src, mesh, error, visible });
            }
        }
        Ok(LoadedCourse {
            course: c.name.clone(),
            variant: v.no,
            variants,
            levels,
            level_errors,
            models,
            load_ms: t0.elapsed().as_millis(),
        })
    }
}

/// Sky domes and lighting helper volumes would hide the course from an outside camera; start them hidden.
pub fn default_visible(name: &str) -> bool {
    let n = name.to_lowercase();
    !(n.contains("_sky") || n.contains("lightingmodel"))
}

impl LoadedCourse {
    pub fn map_level(&self) -> Option<&Level> {
        self.levels.iter().find(|l| l.name.starts_with("LV_MAP")).or_else(|| self.levels.iter().find(|l| l.rail.is_some()))
    }
    pub fn rail(&self) -> Option<&level::PathData> {
        self.levels.iter().find_map(|l| l.rail.and_then(|r| l.path(r)))
    }
}
