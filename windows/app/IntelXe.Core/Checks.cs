// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

public enum CheckState { Good, Warning, Bad, Neutral }

/// One line of the Overview's "This PC" list: what it is, its value, its state and one line on what it means.
public sealed record Check(string Id, Text Title, Text Value, CheckState State, Text Help);

/// How the Overview judges this PC. The limits and wording come from the guide (site/guide/hardware.html, windows.html)
/// and release/README-INSTALL.md stage 1: an Intel CPU, a B580 (tested) or B570 (untested), the firmware in UEFI mode
/// with CSM off, Above 4G decoding and Resizable BAR on; OpenCore does not start under Secure Boot unless signed.
public static class PcChecks
{
    public static IReadOnlyList<Check> Evaluate(PcFacts f)
    {
        var list = new List<Check> { CardCheck(f), CpuCheck(f), HybridCheck(f) };
        list.Add(new Check("board", Text.Of("Check_Board"),
            string.IsNullOrWhiteSpace(f.BoardModel) ? Text.Of("Value_NotReported") : Text.Raw($"{f.BoardMaker} {f.BoardModel}".Trim()),
            CheckState.Neutral, Text.Of("Board_Help")));
        list.Add(FirmwareCheck(f));
        list.Add(SecureBootCheck(f));
        list.Add(RebarCheck(f));
        list.Add(new Check("windows", Text.Of("Check_Windows"),
            f.Windows is null ? Text.Of("Value_Unknown") : f.Windows.BuildText == "" ? Text.Raw(f.Windows.NameAndVersion)
                : Text.Of("Windows_Value", f.Windows.NameAndVersion, f.Windows.BuildText),
            f.Windows is null ? CheckState.Neutral : CheckState.Good, Text.Empty));
        list.Add(BitLockerCheck(f));
        list.Add(EspCheck(f));
        return list;
    }

    static Check CardCheck(PcFacts f)
    {
        var title = Text.Of("Check_Card");
        if (f.Card is { Id: not null } gpu)
        {
            var card = SupportedCards.Find(gpu.Id)!;
            var value = Text.Raw($"{card.Name} ({gpu.Id.VendorDevice})");
            return card.Tested
                ? new Check("card", title, value, CheckState.Good, Text.Empty)
                : new Check("card", title, value, CheckState.Warning, Text.Of("Card_Help_Untested"));
        }
        if (f.OtherIntelGpu is { Id: not null } other)
            return new Check("card", title, Text.Of("Card_None"), CheckState.Bad, Text.Of("Card_Help_Other", $"{other.Name} ({other.Id.VendorDevice})"));
        return new Check("card", title, Text.Of("Card_None"), CheckState.Bad, Text.Of("Card_Help_None"));
    }

    static Check CpuCheck(PcFacts f)
    {
        var name = CpuInfo.CleanName(f.CpuName);
        if (name == "") return new Check("cpu", Text.Of("Check_Cpu"), Text.Of("Value_NotReported"), CheckState.Neutral, Text.Of("Cpu_Help_Unknown"));
        var family = CpuInfo.Family(f.CpuName);
        Text help = family.Vendor == CpuVendor.Intel ? family.Describe() : Text.Empty;
        if (f.PerformanceCores is int p && f.EfficiencyCores is int e && e > 0)
            help = help.IsEmpty ? Text.Of("Cpu_Cores", p, e) : Text.Of("Cpu_FamilyCores", help, p, e);
        return family.Vendor switch
        {
            CpuVendor.Intel => new Check("cpu", Text.Of("Check_Cpu"), Text.Raw(name), CheckState.Good, help),
            CpuVendor.Amd => new Check("cpu", Text.Of("Check_Cpu"), Text.Raw(name), CheckState.Warning, Text.Of("Cpu_Help_Amd")),
            _ => new Check("cpu", Text.Of("Check_Cpu"), Text.Raw(name), CheckState.Warning, Text.Of("Cpu_Help_Other")),
        };
    }

    static Check HybridCheck(PcFacts f) =>
        f.HybridCpu
            ? new Check("hybridpm", Text.Of("Check_HybridPM"), Text.Of("HybridPM_Needed"), CheckState.Neutral, Text.Of("HybridPM_Help_Needed"))
            : new Check("hybridpm", Text.Of("Check_HybridPM"), Text.Of("HybridPM_NotNeeded"), CheckState.Neutral, Text.Of("HybridPM_Help_NotNeeded"));

    static Check FirmwareCheck(PcFacts f) => f.Firmware switch
    {
        FirmwareMode.Uefi => new Check("firmware", Text.Of("Check_Firmware"), Text.Of("Firmware_Uefi"), CheckState.Good, Text.Empty),
        FirmwareMode.Legacy => new Check("firmware", Text.Of("Check_Firmware"), Text.Of("Firmware_Legacy"), CheckState.Bad, Text.Of("Firmware_Help_Legacy")),
        _ => new Check("firmware", Text.Of("Check_Firmware"), Text.Of("Value_Unknown"), CheckState.Neutral, Text.Empty),
    };

    static Check SecureBootCheck(PcFacts f) => f.SecureBoot switch
    {
        SecureBootState.Off => new Check("secureboot", Text.Of("Check_SecureBoot"), Text.Of("State_Off"), CheckState.Good, Text.Empty),
        SecureBootState.On => new Check("secureboot", Text.Of("Check_SecureBoot"), Text.Of("State_On"), CheckState.Warning, Text.Of("SecureBoot_Help_On")),
        SecureBootState.NotSupported => new Check("secureboot", Text.Of("Check_SecureBoot"), Text.Of("SecureBoot_NotSupported"), CheckState.Neutral, Text.Empty),
        _ => new Check("secureboot", Text.Of("Check_SecureBoot"), Text.Of("Value_Unknown"), CheckState.Neutral, Text.Empty),
    };

    static Check RebarCheck(PcFacts f)
    {
        if (f.Bar is null)
            return new Check("rebar", Text.Of("Check_Rebar"), Text.Of("Value_CouldNotRead"), CheckState.Neutral, Text.Of("Rebar_Help_Unknown"));
        if (f.Bar.ResizableBar)
            return new Check("rebar", Text.Of("Check_Rebar"), Text.Of("Rebar_On", f.Bar.LargestMb), CheckState.Good,
                f.Bar.Above4G ? Text.Of("Rebar_Help_Above4G") : Text.Empty);
        return new Check("rebar", Text.Of("Check_Rebar"), Text.Of("Rebar_Off", f.Bar.LargestMb), CheckState.Warning, Text.Of("Rebar_Help_Off"));
    }

    static Check BitLockerCheck(PcFacts f)
    {
        var title = Text.Of("Check_BitLocker");
        return f.BitLocker switch
        {
            BitLockerState.Off => new Check("bitlocker", title, Text.Of("State_Off"), CheckState.Good, Text.Empty),
            BitLockerState.Unknown => new Check("bitlocker", title, Text.Of("Value_CouldNotRead"), CheckState.Neutral, Text.Of("BitLocker_Help_Unknown")),
            BitLockerState.WaitingForActivation => new Check("bitlocker", title, Text.Of("BitLocker_Waiting"), CheckState.Neutral, Text.Of("BitLocker_Help_Waiting")),
            var s => new Check("bitlocker", title, Text.Of("BitLocker_" + s), CheckState.Warning, Text.Of("BitLocker_Help_On")),
        };
    }

    static Check EspCheck(PcFacts f)
    {
        var title = Text.Of("Check_Esp");
        if (f.EspError is not null) return new Check("esp", title, Text.Of("Value_CouldNotRead"), CheckState.Neutral, Text.Raw(f.EspError));
        int n = f.Esps.Count, oc = f.Esps.Count(e => e.OpenCore == OpenCoreState.Found), unread = f.Esps.Count(e => e.OpenCore == OpenCoreState.NeedsMount);
        if (n == 0) return new Check("esp", title, Text.Of("Esp_None"), CheckState.Neutral, Text.Of("Esp_Help_None"));
        if (oc > 0) return new Check("esp", title, Text.Of("Esp_CountOc", n, oc), CheckState.Good, Text.Empty);
        if (unread > 0) return new Check("esp", title, Text.Of("Esp_CountUnread", n, unread), CheckState.Neutral, Text.Of("Esp_Help_Unread"));
        return new Check("esp", title, Text.Of("Esp_Count", n), CheckState.Neutral, Text.Of("Esp_Help_NoOpenCore"));
    }

    /// The worst state in the list (Neutral counts as Good).
    public static CheckState Overall(IEnumerable<Check> checks)
    {
        var states = checks.Select(c => c.State).ToList();
        if (states.Contains(CheckState.Bad)) return CheckState.Bad;
        if (states.Contains(CheckState.Warning)) return CheckState.Warning;
        return CheckState.Good;
    }

    /// Every string key the checks can use, for the tests that hold the string tables to them.
    public static IReadOnlyList<string> Keys { get; } =
    [
        "Check_Card", "Check_Cpu", "Check_HybridPM", "Check_Board", "Check_Firmware", "Check_SecureBoot", "Check_Rebar",
        "Check_Windows", "Check_BitLocker", "Check_Esp",
        "Value_Unknown", "Value_NotReported", "Value_CouldNotRead", "State_On", "State_Off",
        "Card_None", "Card_Help_None", "Card_Help_Untested", "Card_Help_Other",
        "Cpu_Help_Unknown", "Cpu_Help_Amd", "Cpu_Help_Other", "Cpu_Cores", "Cpu_FamilyCores", "Cpu_Gen", "Cpu_WithCodename",
        "HybridPM_Needed", "HybridPM_NotNeeded", "HybridPM_Help_Needed", "HybridPM_Help_NotNeeded",
        "Board_Help", "Firmware_Uefi", "Firmware_Legacy", "Firmware_Help_Legacy",
        "SecureBoot_Help_On", "SecureBoot_NotSupported",
        "Rebar_On", "Rebar_Off", "Rebar_Help_Off", "Rebar_Help_Unknown", "Rebar_Help_Above4G",
        "Windows_Value",
        "BitLocker_On", "BitLocker_Encrypting", "BitLocker_Decrypting", "BitLocker_Paused", "BitLocker_Locked", "BitLocker_Waiting",
        "BitLocker_Help_On", "BitLocker_Help_Unknown", "BitLocker_Help_Waiting",
        "Esp_None", "Esp_Count", "Esp_CountOc", "Esp_CountUnread", "Esp_Help_None", "Esp_Help_Unread", "Esp_Help_NoOpenCore",
    ];
}
