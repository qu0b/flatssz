package ssz;

/**
 * Runtime exception for SSZ encoding/decoding errors.
 */
public class SszError extends RuntimeException {

    public SszError(String message) {
        super(message);
    }

    public static SszError bufferTooSmall() {
        return new SszError("ssz: buffer too small");
    }

    public static SszError invalidOffset() {
        return new SszError("ssz: invalid offset");
    }

    public static SszError invalidBool() {
        return new SszError("ssz: invalid bool value");
    }
}
