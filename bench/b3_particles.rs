#[repr(C)]
#[derive(Clone, Copy)]
struct P {
    x: f64,
    y: f64,
    vx: f64,
    vy: f64,
}

static mut PARTS: [P; 1000000] = [P { x: 0.0, y: 0.0, vx: 0.0, vy: 0.0 }; 1000000];

#[inline]
fn accel(d: f64) -> f64 {
    d * 9.8 - 0.01 * d * d
}

fn main() {
    const N: usize = 1_000_000;
    const DT: f64 = 0.000001;
    unsafe {
        let mut i = 0usize;
        while i < N {
            PARTS[i].x = i as f64 * 0.001;
            PARTS[i].y = ((i & 1023) as f64) * 0.5;
            PARTS[i].vx = 1.0;
            PARTS[i].vy = 0.5;
            i += 1;
        }
        let mut s = 0i32;
        while s < 100 {
            let mut i = 0usize;
            while i < N {
                PARTS[i].vx += accel(PARTS[i].x) * DT;
                PARTS[i].vy += accel(PARTS[i].y) * DT;
                PARTS[i].x += PARTS[i].vx * DT;
                PARTS[i].y += PARTS[i].vy * DT;
                i += 1;
            }
            s += 1;
        }
        let mut acc = 0f64;
        let mut i = 0usize;
        while i < N {
            acc += PARTS[i].x;
            i += 1;
        }
        println!("acc={:.6}", acc);
    }
}
