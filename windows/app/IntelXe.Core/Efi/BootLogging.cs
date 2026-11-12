// SPDX-License-Identifier: MIT
namespace IntelXe.Core.Efi;

/// Boot logging on an EFI, as its status says (boot_logging of IntelXe-EFI.ps1 -Status and efi.sh --status): on (the
/// bootlog change has nothing left to do), off (bootlog-off has nothing left to do), partly on, Misc > Debug missing (no
/// change can be made), or not known (the EFI was not read, or could not be).
public enum BootLogState { Unknown, Missing, Off, Partial, On }

/// The EFI page's boot logging section: what it shows and which of its two changes it offers.
///
/// Turning it off is its own change (EfiChange.BootLogOff), never undo: undo puts back the newest backup's whole
/// config.plist, which is the boot logging change's only while nothing was changed after it (stage 4 after the logs were
/// turned on, or an edit in another tool, would be undone instead, or lost). The off change touches only the log file
/// bit of Misc > Debug > Target, AppleDebug and ApplePanic, and has its own backup that undo can take back.
public static class BootLogging
{
    public static BootLogState State(EfiStatus? status) => status?["boot_logging"] switch
    {
        "on" => BootLogState.On,
        "off" => BootLogState.Off,
        "partial" => BootLogState.Partial,
        "missing" => BootLogState.Missing,
        _ => BootLogState.Unknown,
    };

    /// "Turn on" unless it is on, or cannot be (no Misc > Debug). Not known: offered, and the plan says what it would do.
    public static bool CanTurnOn(BootLogState s) => s is BootLogState.Unknown or BootLogState.Off or BootLogState.Partial;

    /// "Turn off" unless it is off, or cannot be.
    public static bool CanTurnOff(BootLogState s) => s is BootLogState.Unknown or BootLogState.On or BootLogState.Partial;

    /// The key of the state's text in the app's string tables.
    public static string Key(BootLogState s) => s switch
    {
        BootLogState.On => "Efi_BootLog_On",
        BootLogState.Off => "Efi_BootLog_Off",
        BootLogState.Partial => "Efi_BootLog_Partial",
        BootLogState.Missing => "Efi_BootLog_Missing",
        _ => "Efi_BootLog_Unknown",
    };

    /// The keys Key can give (the string tables must have them all).
    public static IReadOnlyList<string> Keys { get; } = Enum.GetValues<BootLogState>().Select(Key).ToList();
}
