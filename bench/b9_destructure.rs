struct Point {
    x: i64,
    y: i64,
}

struct Pair {
    first: i64,
    second: i64,
}

fn main() {
    const ROUNDS: i64 = 50_000_000;
    let mut acc: i64 = 123456789;

    for r in 0..ROUNDS {
        let pt = Point {
            x: (acc ^ r) & 65535,
            y: ((acc >> 16) ^ r) & 65535,
        };

        // 1. Named struct destructuring
        let Point { x, y } = pt;

        // 2. Renamed struct destructuring
        let Point { x: px, y: py } = pt;

        // 3. Inferred struct destructuring
        let tmp = Point { x: px + 1, y: py + 2 };
        let Point { x: ix, y: iy } = tmp;

        // 4. Positional struct destructuring (tuple)
        let p = Pair { first: x * 3 + iy, second: y * 5 + ix };
        let (p1, p2) = (p.first, p.second);

        // 5. Positional array destructuring
        let arr = [p1 ^ p2, p1 + p2];
        let [a0, a1] = arr;

        acc = (acc * 31 + a0 + a1) & 2147483647;
    }

    println!("acc={}", acc);
}
