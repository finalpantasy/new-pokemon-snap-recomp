//! `levelb` reader (FlatBuffers, version 6). Field meaning comes from analysis of LC_001_A (Florio):
//!
//! - root slot 1 = entry name, slot 2 = entities (`{components[], hash}`), slot 3 = scene nodes
//!   (`{components[], node_id (s1), entity_hash (s2), s4}`), slot 6 = course parameters whose s0 is the scene
//!   node of the NEO-ONE rail, slot 11 = species numbers used in the map, slot 13 = environment parameters.
//! - a component is a table with a tag byte in s0 and exactly one sub-table in a higher slot; the slot says the
//!   kind: 5 model instance (name, pos, quat, scale), 6 character template (skeleton, animation archive),
//!   8 path (segments of points), 9 character name (`CH_nnn`), 10 spawn (pos, quat, records -> template + path node).
//!
//! Rail check: Florio leg 1 rail (s6.s0 -> node -> path) measures 115.7 units; the runtime log reads 0..116.

use crate::fb::{Buf, Table};
use std::collections::{BTreeMap, HashMap};

#[derive(Clone, Debug)]
pub struct ModelInstance {
    pub name: String,
    pub pos: [f32; 3],
    pub rot: [f32; 4],
    pub scale: [f32; 3],
}

#[derive(Clone, Debug)]
pub struct PathData {
    pub entity: u32,
    pub segments: Vec<Vec<[f32; 3]>>,
}

impl PathData {
    pub fn length(&self, seg: usize) -> f32 {
        self.segments
            .get(seg)
            .map(|s| s.windows(2).map(|w| dist(w[0], w[1])).sum())
            .unwrap_or(0.0)
    }
    pub fn start(&self) -> Option<[f32; 3]> {
        self.segments.iter().find_map(|s| s.first().copied())
    }
}

#[derive(Clone, Debug, Default)]
pub struct CharTemplate {
    pub ch_name: String,
    pub skeleton: String,
    pub anim: String,
}

#[derive(Clone, Debug)]
pub struct Spawn {
    pub entity: u32,
    pub pos: [f32; 3],
    pub rot: [f32; 4],
    /// Position came from the linked path's first point because the spawn's own position is the origin.
    pub pos_from_path: bool,
    /// Template entity hashes of the spawn records.
    pub templates: Vec<u32>,
    /// Path entity of the first record that names a scene node, if any.
    pub path: Option<u32>,
    pub label: String,
}

#[derive(Clone, Debug, Default)]
pub struct Level {
    pub name: String,
    pub file: String,
    pub size: usize,
    pub entity_count: usize,
    pub node_count: usize,
    pub component_kinds: BTreeMap<String, usize>,
    pub models: Vec<ModelInstance>,
    pub paths: Vec<PathData>,
    pub templates: HashMap<u32, CharTemplate>,
    pub spawns: Vec<Spawn>,
    pub rail: Option<u32>,
    pub species: Vec<u32>,
    pub env_params: Vec<String>,
}

pub fn dist(a: [f32; 3], b: [f32; 3]) -> f32 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
}

pub fn kind_name(slot: usize) -> &'static str {
    match slot {
        1 => "light/probe (s1)",
        4 => "transform",
        5 => "model instance",
        6 => "character template",
        7 => "collision/trigger shape",
        8 => "path",
        9 => "character name",
        10 => "spawn",
        11 => "character behaviour",
        14 => "effect",
        15 => "decal/texture (s15)",
        _ => "unnamed",
    }
}

/// Returns (slot, sub-table) of a component.
fn component_body<'a>(c: &Table<'a>) -> Option<(usize, Table<'a>)> {
    (1..c.slot_count()).find_map(|s| c.table(s).map(|t| (s, t)))
}

pub fn parse(bytes: &[u8], file: &str) -> Result<Level, String> {
    let buf = Buf(bytes);
    let root = buf.root().ok_or("not a FlatBuffers file")?;
    let version = root.u32(0).unwrap_or(0);
    if version != 6 {
        return Err(format!("levelb version {version}, expected 6"));
    }
    let mut lv = Level {
        name: root.str(1).unwrap_or("?").to_string(),
        file: file.to_string(),
        size: bytes.len(),
        ..Default::default()
    };
    let entities = root.tables(2);
    let nodes = root.tables(3);
    lv.entity_count = entities.len();
    lv.node_count = nodes.len();

    // node id -> entity hash
    let node_entity: HashMap<u32, u32> =
        nodes.iter().filter_map(|n| Some((n.u32(1)?, n.u32(2)?))).collect();

    let mut path_by_entity: HashMap<u32, usize> = HashMap::new();
    let mut raw_spawns: Vec<(u32, [f32; 3], [f32; 4], Vec<(u32, Option<u32>)>)> = vec![];

    for e in &entities {
        let hash = e.u32(1).unwrap_or(0);
        let mut tmpl = CharTemplate::default();
        let mut is_tmpl = false;
        for c in e.tables(0) {
            let Some((slot, t)) = component_body(&c) else { continue };
            *lv.component_kinds.entry(format!("{:>2} {}", slot, kind_name(slot))).or_default() += 1;
            match slot {
                5 => lv.models.push(ModelInstance {
                    name: t.str(1).unwrap_or("").to_string(),
                    pos: t.floats::<3>(3).unwrap_or([0.0; 3]),
                    rot: t.floats::<4>(4).unwrap_or([0.0, 0.0, 0.0, 1.0]),
                    scale: t.floats::<3>(5).unwrap_or([1.0; 3]),
                }),
                6 => {
                    is_tmpl = true;
                    tmpl.skeleton = t.str(1).unwrap_or("").to_string();
                    tmpl.anim = t.str(3).unwrap_or("").to_string();
                }
                9 => {
                    is_tmpl = true;
                    tmpl.ch_name = t.str(1).unwrap_or("").to_string();
                }
                8 => {
                    let segments: Vec<Vec<[f32; 3]>> =
                        t.tables(1).iter().map(|s| s.vec3s(1)).filter(|s| !s.is_empty()).collect();
                    if !segments.is_empty() {
                        path_by_entity.insert(hash, lv.paths.len());
                        lv.paths.push(PathData { entity: hash, segments });
                    }
                }
                10 => {
                    let pos = t.floats::<3>(1).unwrap_or([0.0; 3]);
                    let rot = t.floats::<4>(2).unwrap_or([0.0, 0.0, 0.0, 1.0]);
                    let recs = t
                        .tables(3)
                        .iter()
                        .map(|r| (r.u32(0).unwrap_or(0), r.u32(2).and_then(|n| node_entity.get(&n).copied())))
                        .collect();
                    raw_spawns.push((hash, pos, rot, recs));
                }
                _ => {}
            }
        }
        if is_tmpl {
            lv.templates.insert(hash, tmpl);
        }
    }

    for (entity, pos, rot, recs) in raw_spawns {
        let path = recs.iter().find_map(|r| r.1.filter(|p| path_by_entity.contains_key(p)));
        let mut p = pos;
        let mut from_path = false;
        if pos == [0.0; 3] {
            if let Some(start) = path.and_then(|pe| lv.paths[path_by_entity[&pe]].start()) {
                p = start;
                from_path = true;
            }
        }
        let templates: Vec<u32> = recs.iter().map(|r| r.0).collect();
        let mut names: Vec<String> = templates
            .iter()
            .map(|h| match lv.templates.get(h) {
                Some(t) if !t.ch_name.is_empty() => t.ch_name.clone(),
                Some(t) => t.skeleton.clone(),
                None => format!("#{h:08x}"),
            })
            .collect();
        names.dedup();
        lv.spawns.push(Spawn {
            entity,
            pos: p,
            rot,
            pos_from_path: from_path,
            templates,
            path,
            label: names.join(" / "),
        });
    }

    // Rail: course parameters s0 -> scene node -> entity -> path component.
    if let Some(params) = root.table(6) {
        if let Some(node) = params.u32(0) {
            lv.rail = node_entity.get(&node).copied().filter(|e| path_by_entity.contains_key(e));
        }
    }
    lv.species = root.u32s(11);
    if let Some(env) = root.table(13) {
        lv.env_params = env.tables(1).iter().filter_map(|p| p.str(0).map(str::to_string)).collect();
    }
    Ok(lv)
}

impl Level {
    pub fn path(&self, entity: u32) -> Option<&PathData> {
        self.paths.iter().find(|p| p.entity == entity)
    }
    pub fn kind(&self) -> &'static str {
        if self.name.starts_with("LV_ENV") {
            "environment"
        } else if self.name.starts_with("LV_MAP") {
            "map (rail, paths, spawns)"
        } else if self.name.starts_with("LV_SND") {
            "sound"
        } else {
            "other"
        }
    }
}
