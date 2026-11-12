// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

public enum FirmwareMode { Unknown, Uefi, Legacy }

public enum SecureBootState { Unknown, On, Off, NotSupported }

public enum BitLockerState { Unknown, Off, On, Encrypting, Decrypting, Paused, Locked, WaitingForActivation }

public static class BitLocker
{
    /// The shell's System.Volume.BitLockerProtection property of a drive (what Explorer reads, no administrator rights
    /// needed): 1 on, 2 off, 3 encrypting, 4 decrypting, 5 and 7 encryption or decryption paused, 6 on and locked, 8
    /// encrypted but waiting for activation (device encryption whose key is not protected yet); anything else (0, empty:
    /// no BitLocker on this edition or volume) is unknown.
    public static BitLockerState FromShellProperty(object? value)
    {
        int n;
        try { n = value is null ? -1 : Convert.ToInt32(value, System.Globalization.CultureInfo.InvariantCulture); }
        catch (Exception e) when (e is FormatException or InvalidCastException or OverflowException) { return BitLockerState.Unknown; }
        return n switch
        {
            1 => BitLockerState.On,
            2 => BitLockerState.Off,
            3 => BitLockerState.Encrypting,
            4 => BitLockerState.Decrypting,
            5 or 7 => BitLockerState.Paused,
            6 => BitLockerState.Locked,
            8 => BitLockerState.WaitingForActivation,
            _ => BitLockerState.Unknown,
        };
    }

    /// Win32_EncryptableVolume.ProtectionStatus (administrator only): 0 off, 1 on, 2 unknown.
    public static BitLockerState FromProtectionStatus(uint status) => status switch
    {
        0 => BitLockerState.Off,
        1 => BitLockerState.On,
        _ => BitLockerState.Unknown,
    };
}
