fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

#[derive(Clone,Copy)]
struct Node { next:i64, value:i64 }
fn main() {
    let rounds=input(1); let seed=input(2); let n=input(3);
    let mut nodes=[Node{next:0,value:0};4096];
    for i in 0..4096 { nodes[i as usize]=Node{next:((i as i64)*17+1)%n,value:((i as i64)+seed)&65535}; }
    let mut cursor=seed%n; let mut acc=0;
    for r in 0..rounds {
        cursor=nodes[cursor as usize].next;
        nodes[cursor as usize].value=(nodes[cursor as usize].value+acc+r)&65535;
        acc=(acc*31+nodes[cursor as usize].value)&2147483647;
    }
    println!("{} {}",acc,cursor);
}
