//! Minimal schemaless FlatBuffers reader. The game's `levelb`/`courseb` files are FlatBuffers without
//! shipped schemas; slot numbers come from the inferred schemas in `analysis/registries/schemas/`.
//! Every read is bounds-checked: malformed data returns `None` instead of panicking.

#[derive(Clone, Copy)]
pub struct Buf<'a>(pub &'a [u8]);

impl<'a> Buf<'a> {
    pub fn u8_at(&self, p: usize) -> Option<u8> {
        self.0.get(p).copied()
    }
    pub fn u16_at(&self, p: usize) -> Option<u16> {
        Some(u16::from_le_bytes(self.0.get(p..p + 2)?.try_into().ok()?))
    }
    pub fn u32_at(&self, p: usize) -> Option<u32> {
        Some(u32::from_le_bytes(self.0.get(p..p + 4)?.try_into().ok()?))
    }
    pub fn i32_at(&self, p: usize) -> Option<i32> {
        Some(i32::from_le_bytes(self.0.get(p..p + 4)?.try_into().ok()?))
    }
    pub fn f32_at(&self, p: usize) -> Option<f32> {
        Some(f32::from_le_bytes(self.0.get(p..p + 4)?.try_into().ok()?))
    }
    pub fn root(&self) -> Option<Table<'a>> {
        let off = self.u32_at(0)? as usize;
        Table::at(*self, off)
    }
}

#[derive(Clone, Copy)]
pub struct Table<'a> {
    pub buf: Buf<'a>,
    pub pos: usize,
    vt: usize,
    vsize: usize,
}

impl<'a> Table<'a> {
    pub fn at(buf: Buf<'a>, pos: usize) -> Option<Self> {
        let soff = buf.i32_at(pos)? as i64;
        let vt = pos as i64 - soff;
        if vt < 0 {
            return None;
        }
        let vt = vt as usize;
        let vsize = buf.u16_at(vt)? as usize;
        if vsize < 4 || vt + vsize > buf.0.len() {
            return None;
        }
        Some(Self { buf, pos, vt, vsize })
    }

    /// Absolute position of a field, or None when the slot is absent.
    pub fn field(&self, slot: usize) -> Option<usize> {
        let e = 4 + 2 * slot;
        if e + 2 > self.vsize {
            return None;
        }
        let o = self.buf.u16_at(self.vt + e)? as usize;
        if o == 0 {
            None
        } else {
            Some(self.pos + o)
        }
    }

    pub fn has(&self, slot: usize) -> bool {
        self.field(slot).is_some()
    }

    /// Highest slot index the vtable describes (present or not).
    pub fn slot_count(&self) -> usize {
        (self.vsize - 4) / 2
    }

    pub fn u8(&self, slot: usize) -> Option<u8> {
        self.buf.u8_at(self.field(slot)?)
    }
    pub fn u32(&self, slot: usize) -> Option<u32> {
        self.buf.u32_at(self.field(slot)?)
    }
    pub fn f32(&self, slot: usize) -> Option<f32> {
        self.buf.f32_at(self.field(slot)?)
    }
    /// Inline struct of `n` floats.
    pub fn floats<const N: usize>(&self, slot: usize) -> Option<[f32; N]> {
        let p = self.field(slot)?;
        let mut out = [0f32; N];
        for (i, v) in out.iter_mut().enumerate() {
            *v = self.buf.f32_at(p + 4 * i)?;
        }
        Some(out)
    }
    fn indirect(&self, slot: usize) -> Option<usize> {
        let p = self.field(slot)?;
        Some(p + self.buf.u32_at(p)? as usize)
    }
    pub fn str(&self, slot: usize) -> Option<&'a str> {
        let p = self.indirect(slot)?;
        let n = self.buf.u32_at(p)? as usize;
        std::str::from_utf8(self.buf.0.get(p + 4..p + 4 + n)?).ok()
    }
    pub fn table(&self, slot: usize) -> Option<Table<'a>> {
        Table::at(self.buf, self.indirect(slot)?)
    }
    /// Vector header: (position of first element, length).
    pub fn vec(&self, slot: usize) -> Option<(usize, usize)> {
        let p = self.indirect(slot)?;
        let n = self.buf.u32_at(p)? as usize;
        if p + 4 + n > self.buf.0.len() {
            return None;
        }
        Some((p + 4, n))
    }
    pub fn tables(&self, slot: usize) -> Vec<Table<'a>> {
        let Some((p, n)) = self.vec(slot) else { return vec![] };
        (0..n)
            .filter_map(|i| {
                let e = p + 4 * i;
                Table::at(self.buf, e + self.buf.u32_at(e)? as usize)
            })
            .collect()
    }
    pub fn u32s(&self, slot: usize) -> Vec<u32> {
        let Some((p, n)) = self.vec(slot) else { return vec![] };
        (0..n).filter_map(|i| self.buf.u32_at(p + 4 * i)).collect()
    }
    pub fn vec3s(&self, slot: usize) -> Vec<[f32; 3]> {
        let Some((p, n)) = self.vec(slot) else { return vec![] };
        (0..n)
            .filter_map(|i| {
                let e = p + 12 * i;
                Some([self.buf.f32_at(e)?, self.buf.f32_at(e + 4)?, self.buf.f32_at(e + 8)?])
            })
            .collect()
    }
}
