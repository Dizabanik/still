struct Box<T> {
    value: T,
}

impl<T: Copy> Box<T> {
    #[inline(always)]
    fn new(v: T) -> Self {
        Box { value: v }
    }

    #[inline(always)]
    fn get(&self) -> T {
        self.value
    }

    #[inline(always)]
    fn put(&mut self, v: T) {
        self.value = v;
    }
}

fn main() {
    const ROUNDS: i64 = 50_000_000;
    let mut b_i = Box::new(1i64);
    let mut b_w = Box::new(1i32);

    for r in 0..ROUNDS {
        let v = b_i.get();
        let next_i = (v ^ (r * 16777619)) & 2147483647;
        b_i.put(next_i);

        let vw = b_w.get();
        let next_w = ((((vw as i64) * 31 + (r & 255)) ^ (v & 65535)) & 65535) as i32;
        b_w.put(next_w);
    }

    println!("i={} w={}", b_i.get(), b_w.get());
}
