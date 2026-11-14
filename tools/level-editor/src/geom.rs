//! SSBH model reader: `nusrcmdlb` (model) -> `numshb` (mesh) + `nusktb` (skeleton), through ssbh_data (MIT).
//! Produces world-ready triangle soup per model in model space. Unskinned mesh objects are moved by their parent
//! bone's world transform, as `scripts/hd/ssbh_to_gltf` does. No textures yet (flat colour per model).

use glam::{Mat3, Mat4, Vec3};
use ssbh_data::mesh_data::VectorData;
use ssbh_data::prelude::*;
use std::path::Path;

#[derive(Clone, Default)]
pub struct ModelMesh {
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub indices: Vec<u32>,
    pub objects: usize,
    pub mesh_file: String,
}

fn vec3s(d: &VectorData) -> Vec<Vec3> {
    match d {
        VectorData::Vector2(v) => v.iter().map(|a| Vec3::new(a.x, a.y, 0.0)).collect(),
        VectorData::Vector3(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
        VectorData::Vector4(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
    }
}

pub fn load_model(modl_path: &Path) -> Result<ModelMesh, String> {
    let dir = modl_path.parent().ok_or("no parent dir")?;
    let modl = ModlData::from_file(modl_path).map_err(|e| format!("modl {}: {e}", modl_path.display()))?;
    let mesh = MeshData::from_file(dir.join(&modl.mesh_file_name))
        .map_err(|e| format!("mesh {}: {e}", modl.mesh_file_name))?;
    let skel = SkelData::from_file(dir.join(&modl.skeleton_file_name)).ok();
    let bone_world = |name: &str| -> Mat4 {
        skel.as_ref()
            .and_then(|s| s.bones.iter().find(|b| b.name == name).map(|b| (s, b)))
            .and_then(|(s, b)| s.calculate_world_transform(b).ok())
            .unwrap_or(Mat4::IDENTITY)
    };
    let mut out = ModelMesh { mesh_file: modl.mesh_file_name.clone(), ..Default::default() };
    for o in &mesh.objects {
        let Some(pos) = o.positions.first() else { continue };
        let pos = vec3s(&pos.data);
        let m = if o.bone_influences.is_empty() { bone_world(&o.parent_bone_name) } else { Mat4::IDENTITY };
        let nm = Mat3::from_mat4(m).inverse().transpose();
        let base = out.positions.len() as u32;
        let nrm = o.normals.first().map(|n| vec3s(&n.data));
        for (i, p) in pos.iter().enumerate() {
            let q = m.transform_point3(*p);
            out.positions.push(q.to_array());
            let n = nrm.as_ref().and_then(|n| n.get(i)).map(|n| (nm * *n).normalize_or_zero()).unwrap_or(Vec3::Y);
            out.normals.push(n.to_array());
        }
        let n = pos.len() as u32;
        out.indices.extend(o.vertex_indices.iter().filter(|&&i| i < n).map(|i| base + i));
        // keep whole triangles only
        let extra = out.indices.len() % 3;
        out.indices.truncate(out.indices.len() - extra);
        out.objects += 1;
    }
    Ok(out)
}
