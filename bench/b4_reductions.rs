// Rust twin of b4_reductions.wky: same reduction mix over fixed arrays --
// i32 sum, f32 dot accumulated in f64, i32 max.
static mut XI: [i32; 262144] = [0; 262144];
static mut XF: [f32; 131072] = [0.0; 131072];

fn main() {
    unsafe {
        let xi: *mut i32 = std::ptr::addr_of_mut!(XI).cast();
        let xf: *mut f32 = std::ptr::addr_of_mut!(XF).cast();
        let n_xi: usize = 262144;
        let n_xf: usize = 131072;
        // init
        let mut i = 0usize;
        while i < n_xi {
            (*xi.add(i)) = (i as i32 % 977) as i32;
            i += 1;
        }
        i = 0usize;
        while i < n_xf {
            (*xf.add(i)) = (i as f32) * 0.001;
            i += 1;
        }
        let mut total: i64 = 0;
        for _ in 0..2000 {
            let mut a: i64 = 0;
            let mut i = 0usize;
            while i < n_xi {
                a += (*xi.add(i)) as i64;
                i += 1;
            }
            total += a;
        }
        let mut d: f32 = 0.0;
        for _ in 0..4000 {
            let mut acc: f64 = 0.0;
            let mut i = 0usize;
            while i < n_xf {
                acc += ((*xf.add(i)) as f64) * ((*xf.add(i)) as f64);
                i += 1;
            }
            d += acc as f32;
        }
        let mut mx: i32 = 0;
        for _ in 0..1000 {
            let mut m = *xi;
            let mut i = 1usize;
            while i < n_xi {
                let v = *xi.add(i);
                if v > m { m = v; }
                i += 1;
            }
            mx += m;
        }
        println!("t={} d={:.1} m={}", total, d, mx);
    }
}
