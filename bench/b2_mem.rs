static mut DATA: [u32; 67108864] = [0; 67108864];

fn main() {
    let n: usize = 67108864;
    unsafe {
        let mut i = 0usize;
        while i < n {
            DATA[i] = (i as u32).wrapping_mul(2654435761);
            i += 1;
        }
        let mut sum: u64 = 0;
        let mut i = 0usize;
        while i < n {
            sum += DATA[i] as u64;
            i += 1;
        }
        println!("sum={}", sum);
    }
}
