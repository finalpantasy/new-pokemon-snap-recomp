// DRP per-chunk xorshift stream decryptor (public NewSnap algorithm). Source only; build:
//   rustc -O --crate-type cdylib -o tools/oodle_build/drp_xor.dll scripts/drp_xor/drp_xor.rs
#[no_mangle]
pub unsafe extern "C" fn drp_xor(buf: *mut u32, nwords: usize, st: *const u32) {
    let (mut x, mut y, mut z, mut w) = (*st, *st.add(1), *st.add(2), *st.add(3));
    #[inline(always)]
    fn nxt(x: &mut u32, y: &mut u32, z: &mut u32, w: &mut u32) -> u32 {
        let t = *x ^ (*x << 11);
        *x = *y; *y = *z; *z = *w;
        *w = *w ^ (((*w as i32) >> 19) as u32) ^ (t ^ (((t as i32) >> 8) as u32));
        *w & 0x7FFF_FFFF
    }
    for i in 1..nwords {
        let a = nxt(&mut x, &mut y, &mut z, &mut w);
        let b = nxt(&mut x, &mut y, &mut z, &mut w) >> 30;
        *buf.add(i) ^= a | (b << 31);
    }
}
