// SPDX-License-Identifier: MIT
// What both erase dialogs (the installer stick and the probe stick) add to the typed name and size: a stick that holds an
// EFI (StickRules.HoldsEfi: an EFI system partition, or a volume with OpenCore or another EFI boot loader, such as the
// recovery stick that starts a PC) gets a warning of its own and a box the user must tick, and the helper is told it was
// ticked (--erase-efi; without it the helper refuses the disk). A probe stick gets a note that its reports go with it.
using IntelXe.Core.Stick;
using Microsoft.UI.Xaml.Controls;

namespace IntelXe.App;

static class EraseWarnings
{
    /// Adds the EFI warning and its box to the dialog's panel; the box, or null when the disk holds no EFI.
    public static CheckBox? AddEfi(Panel p, StickDisk disk, Action changed)
    {
        if (!StickRules.HoldsEfi(disk)) return null;
        p.Children.Add(new InfoBar
        {
            Severity = InfoBarSeverity.Error, IsOpen = true, IsClosable = false,
            Title = L.Get("Erase_Efi_Title"), Message = L.Get("Erase_Efi_Detail"),
        });
        var box = new CheckBox { Content = L.Get("Erase_Efi_Confirm"), IsChecked = false };
        box.Checked += (_, _) => changed();
        box.Unchecked += (_, _) => changed();
        p.Children.Add(box);
        return box;
    }

    /// Adds the note that a probe stick's reports are erased with it.
    public static void AddProbe(Panel p, StickDisk disk)
    {
        if (!StickRules.IsProbeStick(disk)) return;
        p.Children.Add(new InfoBar
        {
            Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false,
            Title = L.Get("Erase_Probe_Title"), Message = L.Get("Erase_Probe_Detail"),
        });
    }

    /// Whether the EFI box (if any) is ticked.
    public static bool Confirmed(CheckBox? efi) => efi is null || efi.IsChecked == true;
}
