import flatbuffers_codegen.SignedBeaconBlock;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

public class Bench {

    private static final String SSZ_PATH =
        "/home/framework/repos/ssz-benchmark/res/block-mainnet.ssz";
    private static final String META_PATH =
        "/home/framework/repos/ssz-benchmark/res/block-mainnet-meta.json";

    private static final int WARMUP = 1000;
    private static final int ITERS  = 10000;

    public static void main(String[] args) throws IOException {
        byte[] sszData = Files.readAllBytes(Paths.get(SSZ_PATH));
        String metaJson = new String(Files.readAllBytes(Paths.get(META_PATH)));
        String expectedHTR = parseHTR(metaJson);

        System.out.println("SSZ data size: " + sszData.length + " bytes");
        System.out.println("Expected HTR:  " + expectedHTR);

        // Correctness check
        SignedBeaconBlock block = SignedBeaconBlock.fromSSZBytes(sszData);
        // The meta.json HTR is for BeaconBlock (the message), not SignedBeaconBlock
        byte[] htr = block.message.hashTreeRoot();
        String htrHex = bytesToHex(htr);
        if (!htrHex.equals(expectedHTR)) {
            System.err.println("HTR MISMATCH!");
            System.err.println("  got:      " + htrHex);
            System.err.println("  expected: " + expectedHTR);
            System.exit(1);
        }
        System.out.println("HTR verified: " + htrHex);

        // Marshal round-trip check
        byte[] marshaled = block.marshalSSZ();
        if (!Arrays.equals(marshaled, sszData)) {
            System.err.println("MARSHAL MISMATCH! length got=" + marshaled.length
                + " expected=" + sszData.length);
            System.exit(1);
        }
        System.out.println("Marshal round-trip verified");
        System.out.println();

        // Warmup
        System.out.println("Warming up (" + WARMUP + " iterations)...");
        for (int i = 0; i < WARMUP; i++) {
            SignedBeaconBlock.fromSSZBytes(sszData);
        }
        for (int i = 0; i < WARMUP; i++) {
            block.marshalSSZ();
        }
        for (int i = 0; i < WARMUP; i++) {
            block.hashTreeRoot();
        }

        // Benchmark unmarshal
        long start = System.nanoTime();
        for (int i = 0; i < ITERS; i++) {
            SignedBeaconBlock.fromSSZBytes(sszData);
        }
        long elapsed = System.nanoTime() - start;
        System.out.println("unmarshal: " + (elapsed / ITERS / 1000) + " us/op");

        // Benchmark marshal
        start = System.nanoTime();
        for (int i = 0; i < ITERS; i++) {
            block.marshalSSZ();
        }
        elapsed = System.nanoTime() - start;
        System.out.println("marshal: " + (elapsed / ITERS / 1000) + " us/op");

        // Benchmark hashTreeRoot
        start = System.nanoTime();
        for (int i = 0; i < ITERS; i++) {
            block.hashTreeRoot();
        }
        elapsed = System.nanoTime() - start;
        System.out.println("hash_tree_root: " + (elapsed / ITERS / 1000) + " us/op");
    }

    private static String parseHTR(String json) {
        // Minimal JSON parser for {"htr": "..."}
        int idx = json.indexOf("\"htr\"");
        if (idx < 0) throw new RuntimeException("no htr field in meta JSON");
        int colon = json.indexOf(':', idx);
        int q1 = json.indexOf('"', colon + 1);
        int q2 = json.indexOf('"', q1 + 1);
        return json.substring(q1 + 1, q2);
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format("%02x", b & 0xFF));
        }
        return sb.toString();
    }
}
