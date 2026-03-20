using System;
using System.IO;
using System.Diagnostics;
using System.Text.Json;
using SszFlatbuffers;
using flatbuffers_codegen;

class Bench {
    static void Main() {
        var data = File.ReadAllBytes("block-mainnet.ssz");
        var meta = JsonDocument.Parse(File.ReadAllText("block-mainnet-meta.json"));
        var expectedHtr = Convert.FromHexString(meta.RootElement.GetProperty("htr").GetString()!);
        Console.WriteLine($"SSZ data: {data.Length} bytes");

        var block = SignedBeaconBlock.FromSSZBytes(data);
        var htr = block.Message.HashTreeRoot();
        bool match = true;
        for (int i = 0; i < 32; i++) if (htr[i] != expectedHtr[i]) { match = false; break; }
        Console.WriteLine(match ? "HTR ✓" : "HTR MISMATCH");
        if (!match) return;

        int warmup = 100, N;
        Stopwatch sw;

        for (int i = 0; i < warmup; i++) SignedBeaconBlock.FromSSZBytes(data);
        N = 1000; sw = Stopwatch.StartNew();
        for (int i = 0; i < N; i++) SignedBeaconBlock.FromSSZBytes(data);
        sw.Stop(); Console.WriteLine($"unmarshal: {sw.Elapsed.TotalMicroseconds / N:F0} us/op");

        for (int i = 0; i < warmup; i++) block.MarshalSSZ();
        N = 5000; sw = Stopwatch.StartNew();
        for (int i = 0; i < N; i++) block.MarshalSSZ();
        sw.Stop(); Console.WriteLine($"marshal: {sw.Elapsed.TotalMicroseconds / N:F0} us/op");

        for (int i = 0; i < warmup; i++) block.Message.HashTreeRoot();
        N = 200; sw = Stopwatch.StartNew();
        for (int i = 0; i < N; i++) block.Message.HashTreeRoot();
        sw.Stop(); Console.WriteLine($"hash_tree_root: {sw.Elapsed.TotalMicroseconds / N:F0} us/op");
    }
}
