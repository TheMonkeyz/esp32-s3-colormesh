package android.bluetooth.le;

import android.bluetooth.BluetoothDevice;
import android.os.IInterface;

// Stub of the Bluetooth service's hidden callback interface (framework-bluetooth.jar, AIDL, oneway): the app's own
// Binder implements it (Radio.SyncBinder) and reads the transactions itself, so the real Stub class is never linked.
public interface IPeriodicAdvertisingCallback extends IInterface {
    void onSyncEstablished(int syncHandle, BluetoothDevice device, int advertisingSid, int skip, int timeout,
                           int status);
    void onPeriodicAdvertisingReport(PeriodicAdvertisingReport report);
    void onSyncLost(int syncHandle);
    void onSyncTransferred(BluetoothDevice device, int status);
    void onBigInfoAdvertisingReport(int syncHandle, boolean encrypted);
}
