enum Shape {
    Circle(i64),
    Rect(i64, i64),
    Point,
}

#[inline(always)]
fn eval_shape(s: Shape, factor: i64) -> i64 {
    match s {
        Shape::Circle(radius) => radius * radius * 3 + factor,
        Shape::Rect(width, height) => width * height + factor * 2,
        Shape::Point => factor,
    }
}

fn main() {
    const ROUNDS: i64 = 20_000_000;
    let mut acc: i64 = 0;
    for r in 0..ROUNDS {
        let mode = r & 3;
        let s = if mode == 0 {
            Shape::Circle((r & 127) + 1)
        } else if mode == 1 {
            Shape::Rect((r & 63) + 1, ((r >> 2) & 63) + 1)
        } else if mode == 2 {
            Shape::Point
        } else {
            Shape::Rect(((r >> 1) & 31) + 2, (r & 31) + 3)
        };

        let val = eval_shape(s, r & 15);
        acc = (acc.wrapping_mul(33) + val) & 2147483647;
    }

    println!("acc={}", acc);
}
