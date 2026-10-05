// Rust twin of b6_chan.wky: four-stage pipeline (src -> s1 -> s2 -> drain)
// over capacity-64 channels with blocking send/recv -- expressed with the
// standard library's bounded sync channels and real OS threads, which is
// the idiomatic Rust shape of this program.
use std::sync::mpsc::sync_channel;
use std::thread;

const N: i64 = 2_000_000;
const CAP: usize = 64;

#[inline]
fn step(v: i64) -> i64 {
    (v * 3 + 1) & 2147483647
}

fn main() {
    let (t1_tx, t1_rx) = sync_channel::<i64>(CAP);
    let (t2_tx, t2_rx) = sync_channel::<i64>(CAP);
    let (t3_tx, t3_rx) = sync_channel::<i64>(CAP);

    let src = thread::spawn(move || {
        for i in 0..N {
            t1_tx.send(step(i)).unwrap();
        }
    });
    let s1 = thread::spawn(move || {
        for _ in 0..N {
            let v = t1_rx.recv().unwrap();
            t2_tx.send(step(v)).unwrap();
        }
    });
    let s2 = thread::spawn(move || {
        for _ in 0..N {
            let v = t2_rx.recv().unwrap();
            t3_tx.send(step(v)).unwrap();
        }
    });
    let sink = thread::spawn(move || {
        let mut sum: u64 = 0;
        for _ in 0..N {
            sum += t3_rx.recv().unwrap() as u64;
        }
        sum
    });

    src.join().unwrap();
    s1.join().unwrap();
    s2.join().unwrap();
    let sum = sink.join().unwrap();
    println!("sum={}", sum);
}
