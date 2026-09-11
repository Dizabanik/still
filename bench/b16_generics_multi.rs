struct Pair<A, B> {
    first: A,
    second: B,
}

impl<A, B> Pair<A, B> {
    #[inline(always)]
    fn new(f: A, s: B) -> Self {
        Pair { first: f, second: s }
    }
    #[inline(always)]
    fn get_first(&self) -> A where A: Copy {
        self.first
    }
    #[inline(always)]
    fn get_second(&self) -> B where B: Copy {
        self.second
    }
    #[inline(always)]
    fn swap(self) -> Pair<B, A> {
        Pair { first: self.second, second: self.first }
    }
}

fn main() {
    const ROUNDS: i64 = 20000000;
    let mut p = Pair::new(123456789i64, 42i32);
    let mut acc: i64 = 0;

    for r in 0..ROUNDS {
        let a = p.get_first();
        let b = p.get_second();
        let na = (a ^ (r.wrapping_mul(16777619))) & 2147483647;
        let nb = ((((b as i64) * 31 + (r & 255)) ^ (a & 65535)) & 65535) as i32;
        p = Pair::new(na, nb).swap().swap();
        acc = (acc.wrapping_add(p.get_first()).wrapping_add(p.get_second() as i64)) & 2147483647;
    }

    println!("acc={} a={} b={}", acc, p.get_first(), p.get_second());
}
