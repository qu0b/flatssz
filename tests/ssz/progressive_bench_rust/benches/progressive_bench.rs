use criterion::{criterion_group, criterion_main, Criterion};
use progressive_bench_rust::container::ProgressiveExample;
use progressive_bench_rust::list::ProgressiveListExample;

fn bench_progressive(c: &mut Criterion) {
    // Verify container round-trip + HTR
    let container = ProgressiveExample {
        genesis_time: 1606824023,
        slot: 999999,
        state_root: progressive_bench_rust::container::Bytes32 {
            data: [0xAA; 32],
        },
    };
    let data = container.as_ssz_bytes();
    let decoded = ProgressiveExample::from_ssz_bytes(&data).unwrap();
    assert_eq!(decoded.genesis_time, container.genesis_time);
    assert_eq!(decoded.slot, container.slot);
    let root1 = container.tree_hash_root();
    let root2 = container.tree_hash_root();
    assert_eq!(root1, root2, "progressive container HTR not deterministic");

    // Verify list round-trip + HTR
    let list = ProgressiveListExample {
        values: (0..1000).collect(),
        data: vec![0xFF; 256],
    };
    let list_data = list.as_ssz_bytes();
    let list_decoded = ProgressiveListExample::from_ssz_bytes(&list_data).unwrap();
    assert_eq!(list_decoded.values.len(), 1000);
    let lr1 = list.tree_hash_root();
    let lr2 = list.tree_hash_root();
    assert_eq!(lr1, lr2, "progressive list HTR not deterministic");

    // Benchmarks
    c.bench_function("progressive_container_htr", |b| {
        b.iter(|| container.tree_hash_root());
    });

    c.bench_function("progressive_container_marshal", |b| {
        b.iter(|| container.as_ssz_bytes());
    });

    c.bench_function("progressive_list_htr_1000", |b| {
        b.iter(|| list.tree_hash_root());
    });

    c.bench_function("progressive_list_marshal_1000", |b| {
        b.iter(|| list.as_ssz_bytes());
    });
}

criterion_group!(benches, bench_progressive);
criterion_main!(benches);
