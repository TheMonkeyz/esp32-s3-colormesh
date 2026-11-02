package android.bluetooth.le;

// Stub of the hidden class: one periodic advertising packet (or fragment) from a synced train
public final class PeriodicAdvertisingReport {
    public static final int DATA_COMPLETE = 0;
    public static final int DATA_INCOMPLETE_MORE_TO_COME = 1;
    public static final int DATA_INCOMPLETE_TRUNCATED = 2;

    private PeriodicAdvertisingReport() {}

    public int getSyncHandle() { throw new UnsupportedOperationException("stub"); }
    public int getTxPower() { throw new UnsupportedOperationException("stub"); }
    public int getRssi() { throw new UnsupportedOperationException("stub"); }
    public int getDataStatus() { throw new UnsupportedOperationException("stub"); }
    public ScanRecord getData() { throw new UnsupportedOperationException("stub"); }
    public long getTimestampNanos() { throw new UnsupportedOperationException("stub"); }
}
