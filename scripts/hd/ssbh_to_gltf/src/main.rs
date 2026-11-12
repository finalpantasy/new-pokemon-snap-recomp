// SPDX-License-Identifier: MIT
// ssbh_to_gltf: converts one SSBH model (nusrcmdlb + numshb + nusktb + numatb) to glTF 2.0 (mesh, skeleton bind pose,
// material slots). Textures are referenced by name only (images use relative uri textures/<name>.orig.png).
// usage: ssbh_to_gltf <model.nusrcmdlb> <out.gltf> [--info <info.json>]
// Writes <out>.bin next to the gltf. Prints nothing on success; warnings go to stderr.
use serde_json::{json, Value};
use ssbh_data::mesh_data::VectorData;
use ssbh_data::prelude::*;
use std::collections::HashMap;
use std::path::Path;

struct Bin {
    data: Vec<u8>,
    views: Vec<Value>,
    accs: Vec<Value>,
}
impl Bin {
    fn view(&mut self, bytes: &[u8], target: Option<u32>) -> usize {
        while self.data.len() % 4 != 0 {
            self.data.push(0);
        }
        let off = self.data.len();
        self.data.extend_from_slice(bytes);
        let mut v = json!({"buffer":0,"byteOffset":off,"byteLength":bytes.len()});
        if let Some(t) = target {
            v["target"] = json!(t);
        }
        self.views.push(v);
        self.views.len() - 1
    }
    fn acc(&mut self, bytes: &[u8], target: Option<u32>, ctype: u32, count: usize, ty: &str, mm: Option<(Vec<f32>, Vec<f32>)>) -> usize {
        let bv = self.view(bytes, target);
        let mut a = json!({"bufferView":bv,"componentType":ctype,"count":count,"type":ty});
        if let Some((mn, mx)) = mm {
            a["min"] = json!(mn);
            a["max"] = json!(mx);
        }
        self.accs.push(a);
        self.accs.len() - 1
    }
}
fn f32b(v: &[f32]) -> Vec<u8> {
    v.iter().flat_map(|x| x.to_le_bytes()).collect()
}
fn vec4s(d: &VectorData) -> Vec<[f32; 4]> {
    match d {
        VectorData::Vector2(v) => v.iter().map(|a| [a.x, a.y, 0.0, 0.0]).collect(),
        VectorData::Vector3(v) => v.iter().map(|a| [a.x, a.y, a.z, 0.0]).collect(),
        VectorData::Vector4(v) => v.iter().map(|a| [a.x, a.y, a.z, a.w]).collect(),
    }
}
fn role_of(tex: &str) -> &'static str {
    let t = tex.to_lowercase();
    for (suf, r) in [("_col", "albedo"), ("_dif", "albedo"), ("_nor", "normal"), ("_prm", "mask"), ("_par", "mask"), ("_emi", "emissive")] {
        if t.ends_with(suf) {
            return r;
        }
    }
    "other"
}
fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() == 3 && a[1] == "--anim" {
        // debug: print non-skeletal animation tracks (frame 0 and distinct values)
        use ssbh_data::anim_data::*;
        let an = AnimData::from_file(&a[2]).expect("anim");
        for g in &an.groups {
            if matches!(g.group_type, GroupType::Transform) { continue; }
            for n in &g.nodes {
                for t in &n.tracks {
                    let v = match &t.values {
                        TrackValues::UvTransform(v) => format!("uv {:?}", v.iter().take(3).collect::<Vec<_>>()),
                        TrackValues::Vector4(v) => format!("v4 n={} first={:?}", v.len(), v.first()),
                        TrackValues::Float(v) => format!("f n={} first={:?}", v.len(), v.first()),
                        TrackValues::PatternIndex(v) => format!("pat n={} first={:?} uniq={:?}", v.len(), v.first(), { let mut u = v.clone(); u.sort(); u.dedup(); u }),
                        TrackValues::Boolean(v) => format!("bool n={} first={:?} any={}", v.len(), v.first(), v.iter().any(|x| *x)),
                        _ => "other".into(),
                    };
                    println!("{:?} | {} | {} | {}", g.group_type, n.name, t.name, v);
                }
            }
        }
        return;
    }
    if a.len() < 3 {
        eprintln!("usage: ssbh_to_gltf <model.nusrcmdlb> <out.gltf> [--info info.json]");
        std::process::exit(2);
    }
    let modl_path = Path::new(&a[1]);
    let dir = modl_path.parent().unwrap();
    let modl = ModlData::from_file(modl_path).expect("modl");
    let mesh = MeshData::from_file(dir.join(&modl.mesh_file_name)).expect("mesh");
    let skel = SkelData::from_file(dir.join(&modl.skeleton_file_name)).ok();
    let mut warnings: Vec<String> = vec![];
    // materials
    let mut matls: Vec<ssbh_data::matl_data::MatlEntryData> = vec![];
    for f in &modl.material_file_names {
        match MatlData::from_file(dir.join(f)) {
            Ok(m) => matls.extend(m.entries),
            Err(e) => warnings.push(format!("matl {f}: {e}")),
        }
    }
    let mut gmats: Vec<Value> = vec![];
    let mut mat_idx: HashMap<String, usize> = HashMap::new();
    let mut images: Vec<Value> = vec![];
    let mut img_idx: HashMap<String, usize> = HashMap::new();
    let mut info_mats: Vec<Value> = vec![];
    let mut tex_set: std::collections::BTreeMap<String, String> = Default::default();
    let mut unknown_params: std::collections::BTreeSet<String> = Default::default();
    for m in &matls {
        let mut texs = vec![];
        let mut roles: HashMap<&str, String> = HashMap::new();
        for t in &m.textures {
            let pid = format!("{:?}", t.param_id);
            let r = role_of(&t.data);
            if r == "other" {
                unknown_params.insert(format!("{}={}", pid, t.data));
            }
            texs.push(json!({"param":pid,"name":t.data,"role":r}));
            tex_set.entry(t.data.clone()).or_insert(r.to_string());
            roles.entry(r).or_insert(t.data.clone());
        }
        let mut gm = json!({"name":m.material_label,"doubleSided":true,
            "extras":{"shader":m.shader_label,"textures":texs}});
        let mut img = |name: &str, images: &mut Vec<Value>| -> usize {
            *img_idx.entry(name.to_string()).or_insert_with(|| {
                images.push(json!({"name":name,"uri":format!("textures/{name}.orig.png")}));
                images.len() - 1
            })
        };
        let mut pbr = json!({"metallicFactor":0.0,"roughnessFactor":0.7});
        if let Some(n) = roles.get("albedo") {
            let i = img(n, &mut images);
            pbr["baseColorTexture"] = json!({"index":i});
        }
        if let Some(n) = roles.get("normal") {
            let i = img(n, &mut images);
            gm["normalTexture"] = json!({"index":i});
        }
        if let Some(n) = roles.get("emissive") {
            let i = img(n, &mut images);
            gm["emissiveTexture"] = json!({"index":i});
            gm["emissiveFactor"] = json!([1.0, 1.0, 1.0]);
        }
        gm["pbrMetallicRoughness"] = pbr;
        if let Some(b) = m.blend_states.first() {
            let sc = format!("{:?}", b.data.source_color);
            let dc = format!("{:?}", b.data.destination_color);
            if sc != "One" || dc != "Zero" {
                gm["alphaMode"] = json!("BLEND");
            }
            gm["extras"]["blend"] = json!(format!("{sc}/{dc}"));
        }
        let fl: Vec<Value> = m.floats.iter().map(|p| json!([format!("{:?}", p.param_id), p.data])).collect();
        let vc: Vec<Value> = m.vectors.iter().map(|p| json!([format!("{:?}", p.param_id), [p.data.x, p.data.y, p.data.z, p.data.w]])).collect();
        gm["extras"]["floats"] = json!(fl);
        gm["extras"]["vectors"] = json!(vc);
        mat_idx.insert(m.material_label.clone(), gmats.len());
        info_mats.push(json!({"name":m.material_label,"shader":m.shader_label,"textures":gm["extras"]["textures"],
            "roles":{"albedo":roles.get("albedo"),"normal":roles.get("normal"),"mask":roles.get("mask"),"emissive":roles.get("emissive")}}));
        gmats.push(gm);
    }
    let textures: Vec<Value> = (0..images.len()).map(|i| json!({"source":i})).collect();
    // skeleton
    let mut b = Bin { data: vec![], views: vec![], accs: vec![] };
    let mut nodes: Vec<Value> = vec![];
    let mut world: Vec<glam::Mat4> = vec![];
    let mut bone_idx: HashMap<String, usize> = HashMap::new();
    let mut skin: Option<Value> = None;
    let mut max_recon = 0f32;
    let mut scene_nodes: Vec<usize> = vec![];
    if let Some(sk) = &skel {
        let mut children: Vec<Vec<usize>> = vec![vec![]; sk.bones.len()];
        let mut roots = vec![];
        for (i, bn) in sk.bones.iter().enumerate() {
            bone_idx.insert(bn.name.clone(), i);
            match bn.parent_index {
                Some(p) if p < sk.bones.len() => children[p].push(i),
                _ => roots.push(i),
            }
            let w = sk.calculate_world_transform(bn).unwrap_or(glam::Mat4::IDENTITY);
            world.push(w);
            let (s, r, t) = bn.transform.to_scale_rotation_translation();
            let recon = glam::Mat4::from_scale_rotation_translation(s, r, t);
            let e = recon.to_cols_array().iter().zip(bn.transform.to_cols_array().iter()).map(|(x, y)| (x - y).abs()).fold(0f32, f32::max);
            max_recon = max_recon.max(e);
            let mut n = json!({"name":bn.name,"translation":[t.x,t.y,t.z],"rotation":[r.x,r.y,r.z,r.w]});
            if (s - glam::Vec3::ONE).abs().max_element() > 1e-5 {
                n["scale"] = json!([s.x, s.y, s.z]);
            }
            nodes.push(n);
        }
        for (i, c) in children.iter().enumerate() {
            if !c.is_empty() {
                nodes[i]["children"] = json!(c);
            }
        }
        if max_recon > 1e-3 {
            warnings.push(format!("bone transform TRS decomposition error {max_recon} (shear?)"));
        }
        let ibm: Vec<f32> = world.iter().flat_map(|w| w.inverse().to_cols_array()).collect();
        let acc = b.acc(&f32b(&ibm), None, 5126, world.len(), "MAT4", None);
        scene_nodes = roots.clone();
        skin = Some(json!({"inverseBindMatrices":acc,"joints":(0..sk.bones.len()).collect::<Vec<_>>(),"skeleton":roots.first().copied().unwrap_or(0)}));
    } else {
        warnings.push("no skeleton".into());
    }
    let nbones = nodes.len();
    let mut meshes: Vec<Value> = vec![];
    let mut mesh_info: Vec<Value> = vec![];
    for o in &mesh.objects {
        let pos = match o.positions.first() {
            Some(p) => vec4s(&p.data),
            None => continue,
        };
        let n = pos.len();
        let skinned = !o.bone_influences.is_empty() && skin.is_some();
        let m = if skinned {
            glam::Mat4::IDENTITY
        } else {
            bone_idx.get(&o.parent_bone_name).map(|&i| world[i]).unwrap_or(glam::Mat4::IDENTITY)
        };
        let nm = glam::Mat3::from_mat4(m).inverse().transpose();
        let mut pf = vec![];
        let mut mn = vec![f32::MAX; 3];
        let mut mx = vec![f32::MIN; 3];
        for p in &pos {
            let q = m.transform_point3(glam::Vec3::new(p[0], p[1], p[2]));
            for (k, v) in [q.x, q.y, q.z].iter().enumerate() {
                mn[k] = mn[k].min(*v);
                mx[k] = mx[k].max(*v);
            }
            pf.extend_from_slice(&[q.x, q.y, q.z]);
        }
        let mut attrs = serde_json::Map::new();
        attrs.insert("POSITION".into(), json!(b.acc(&f32b(&pf), Some(34962), 5126, n, "VEC3", Some((mn, mx)))));
        if let Some(a) = o.normals.first() {
            let mut nf = vec![];
            for p in vec4s(&a.data) {
                let q = (nm * glam::Vec3::new(p[0], p[1], p[2])).normalize_or_zero();
                nf.extend_from_slice(&[q.x, q.y, q.z]);
            }
            if nf.len() == n * 3 {
                attrs.insert("NORMAL".into(), json!(b.acc(&f32b(&nf), Some(34962), 5126, n, "VEC3", None)));
            }
        }
        if let Some(a) = o.tangents.first() {
            let mut tf = vec![];
            for p in vec4s(&a.data) {
                let q = (glam::Mat3::from_mat4(m) * glam::Vec3::new(p[0], p[1], p[2])).normalize_or_zero();
                tf.extend_from_slice(&[q.x, q.y, q.z, if p[3] < 0.0 { -1.0 } else { 1.0 }]);
            }
            if tf.len() == n * 4 {
                attrs.insert("TANGENT".into(), json!(b.acc(&f32b(&tf), Some(34962), 5126, n, "VEC4", None)));
            }
        }
        for (si, a) in o.texture_coordinates.iter().take(2).enumerate() {
            let mut uf = vec![];
            for p in vec4s(&a.data) {
                uf.extend_from_slice(&[p[0], p[1]]);
            }
            if uf.len() == n * 2 {
                attrs.insert(format!("TEXCOORD_{si}"), json!(b.acc(&f32b(&uf), Some(34962), 5126, n, "VEC2", None)));
            }
        }
        if o.texture_coordinates.len() > 2 {
            warnings.push(format!("{} has {} UV sets, kept 2", o.name, o.texture_coordinates.len()));
        }
        if skinned {
            let mut per: Vec<Vec<(u16, f32)>> = vec![vec![]; n];
            for bi in &o.bone_influences {
                match bone_idx.get(&bi.bone_name) {
                    Some(&j) => {
                        for w in &bi.vertex_weights {
                            if (w.vertex_index as usize) < n && w.vertex_weight > 0.0 {
                                per[w.vertex_index as usize].push((j as u16, w.vertex_weight));
                            }
                        }
                    }
                    None => warnings.push(format!("{}: influence bone {} not in skeleton", o.name, bi.bone_name)),
                }
            }
            let mut jb = vec![];
            let mut wb = vec![];
            let mut over = 0;
            let mut none = 0;
            for v in per.iter_mut() {
                v.sort_by(|x, y| y.1.partial_cmp(&x.1).unwrap());
                if v.len() > 4 {
                    over += 1;
                    v.truncate(4);
                }
                if v.is_empty() {
                    none += 1;
                }
                let s: f32 = v.iter().map(|x| x.1).sum();
                for k in 0..4 {
                    let (j, w) = v.get(k).copied().unwrap_or((0, 0.0));
                    jb.extend_from_slice(&j.to_le_bytes());
                    wb.push(if s > 0.0 { w / s } else if k == 0 { 1.0 } else { 0.0 });
                }
            }
            if over > 0 {
                warnings.push(format!("{}: {} vertices with >4 influences truncated", o.name, over));
            }
            if none > 0 {
                warnings.push(format!("{}: {} skinned vertices without weights", o.name, none));
            }
            attrs.insert("JOINTS_0".into(), json!(b.acc(&jb, Some(34962), 5123, n, "VEC4", None)));
            attrs.insert("WEIGHTS_0".into(), json!(b.acc(&f32b(&wb), Some(34962), 5126, n, "VEC4", None)));
        }
        let idx: Vec<u8> = o.vertex_indices.iter().flat_map(|x| x.to_le_bytes()).collect();
        let ia = b.acc(&idx, Some(34963), 5125, o.vertex_indices.len(), "SCALAR", None);
        let ent = modl.entries.iter().find(|e| e.mesh_object_name == o.name && e.mesh_object_subindex == o.subindex);
        let mname = ent.map(|e| e.material_label.clone());
        let mut prim = json!({"attributes":attrs,"indices":ia,"mode":4});
        match mname.as_ref().and_then(|l| mat_idx.get(l)) {
            Some(&mi) => prim["material"] = json!(mi),
            None => warnings.push(format!("{} #{}: no material ({:?})", o.name, o.subindex, mname)),
        }
        meshes.push(json!({"name":format!("{}#{}", o.name, o.subindex),"primitives":[prim]}));
        let mut nd = json!({"name":format!("{}#{}", o.name, o.subindex),"mesh":meshes.len() - 1});
        if skinned {
            nd["skin"] = json!(0);
        }
        nodes.push(nd);
        scene_nodes.push(nodes.len() - 1);
        mesh_info.push(json!({"name":o.name,"subindex":o.subindex,"material":mname,"vertices":n,"triangles":o.vertex_indices.len() / 3,
            "skinned":skinned,"parent_bone":o.parent_bone_name,"uv_sets":o.texture_coordinates.len(),"has_tangents":!o.tangents.is_empty(),"has_normals":!o.normals.is_empty()}));
    }
    let out = Path::new(&a[2]);
    let binname = format!("{}.bin", out.file_stem().unwrap().to_string_lossy());
    let mut g = json!({"asset":{"version":"2.0","generator":"ssbh_to_gltf"},"scene":0,"scenes":[{"nodes":scene_nodes}],
        "nodes":nodes,"meshes":meshes,"materials":gmats,"images":images,"textures":textures,
        "accessors":b.accs,"bufferViews":b.views,"buffers":[{"uri":binname,"byteLength":b.data.len()}]});
    if let Some(s) = skin {
        g["skins"] = json!([s]);
    }
    if g["materials"].as_array().unwrap().is_empty() {
        g.as_object_mut().unwrap().remove("materials");
    }
    if images.is_empty() {
        g.as_object_mut().unwrap().remove("images");
        g.as_object_mut().unwrap().remove("textures");
    }
    std::fs::write(out.with_file_name(&binname), &b.data).unwrap();
    std::fs::write(out, serde_json::to_vec(&g).unwrap()).unwrap();
    if let Some(i) = a.iter().position(|x| x == "--info") {
        let info = json!({"model":modl.model_name,"mesh_file":modl.mesh_file_name,"skeleton":modl.skeleton_file_name,"material_files":modl.material_file_names,
            "bones":nbones,"meshes":mesh_info,"materials":info_mats,"textures":tex_set,"unknown_params":unknown_params,"warnings":warnings});
        std::fs::write(&a[i + 1], serde_json::to_vec_pretty(&info).unwrap()).unwrap();
    }
    for w in &warnings {
        eprintln!("warn: {w}");
    }
}
