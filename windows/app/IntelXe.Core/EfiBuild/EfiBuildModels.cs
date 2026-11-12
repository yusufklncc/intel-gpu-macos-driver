// SPDX-License-Identifier: MIT
// The seam between the app and the OpenCore EFI builder (windows/efibuild, Python, driving a pinned OpCore Simplify):
// docs/design/DESIGN-efi-build.md section 5. ScanAsync reads this PC (Hardware Sniffer's report and the ACPI tables the
// build needs, through the elevated helper); BuildAsync builds an EFI from them as the user. The app only calls
// IEfiBuilder; PythonEfiBuilder is the one implementation, tested with a fake Python.
namespace IntelXe.Core.EfiBuild;

/// What a scan left: Hardware Sniffer's Report.json and the folder of the ACPI tables the build reads (DSDT, SSDT, APIC,
/// DMAR only).
public sealed record HardwareScan(string ReportPath, string AcpiDir, IReadOnlyList<string> Tables);

/// One build: the scan's report and tables, an output folder (empty, or an earlier build's), the macOS version ("26"
/// Tahoe, the default, or "15" Sequoia), another Mac model than the policy's (null: MacBookPro16,2), and the Safari
/// setting with stage 2 (true, as the setup assistant's first step).
public sealed record EfiBuildRequest(string ReportPath, string AcpiDir, string OutDir,
    string MacOS = "26", string? Smbios = null, bool Safari = true);

/// One question OpCore Simplify asked and the rule that answered it.
public sealed record EfiBuildDecision(string Rule, string Screen, string Question, string Answer, string Why);

/// One change the builder made after OpCore Simplify (stage 1 as the test machine installed, stage 2, the Safari
/// setting): the step, what changed, and why ("" when the result gives no reason).
public sealed record EfiBuildChange(string Step, string Change, string Why);

/// ocvalidate's verdict on the built config.plist: whether it passed, how many issues it counted, the ones accepted on
/// purpose (with the reason) and the others. Error: it could not run (the build then failed).
public sealed record OcValidateResult(bool Ok, int Found, IReadOnlyList<(string Finding, string Why)> Accepted,
    IReadOnlyList<string> Issues, string? Error = null)
{
    public static readonly OcValidateResult None = new(false, 0, [], [], null);
}

/// What a build did, from the builder's result JSON (every string already free of SMBIOS values: the builder removes
/// them, and PythonEfiBuilder removes them again with the values read from the built config.plist).
public sealed record EfiBuildResult(bool Ok, string? Error, string OpenCore, string Smbios, string MacOS,
    IReadOnlyList<EfiBuildDecision> Decisions, IReadOnlyList<EfiBuildChange> Changes, IReadOnlyList<string> Firmware,
    IReadOnlyList<string> AfterInstall, IReadOnlyList<string> Failures, IReadOnlyList<string> Notes,
    OcValidateResult OcValidate, IReadOnlyList<string> Cards, string Cpu, string LogPath, string ResultJsonPath, string EfiDir)
{
    /// A build that did not get as far as a result: only the error.
    public static EfiBuildResult Failed(string error, string logPath = "", string resultPath = "") =>
        new(false, error, "", "", "", [], [], [], [], [], [], OcValidateResult.None, [], "", logPath, resultPath, "");
}

/// The builder cannot run here (no engine beside the app, a changed file in it, no administrator rights for the scan):
/// the message says why.
public sealed class EfiBuildException(string message) : Exception(message);

public interface IEfiBuilder
{
    /// The engine folder is there (python.exe, efibuild.py, its manifest). The manifest itself is checked before every
    /// scan and build.
    bool IsAvailable { get; }

    /// Why it is not available ("" when it is).
    string Unavailable { get; }

    /// "OpCore Simplify 9aed67e, Hardware Sniffer ff3b9ed, OpenCore 1.0.8, Python 3.12.10".
    string Versions { get; }

    /// Hardware Sniffer's report and the ACPI tables of this PC into outDir (Report.json, ACPI\). Needs administrator
    /// rights: it goes through the elevated helper.
    Task<HardwareScan> ScanAsync(string outDir, IProgress<string> progress, CancellationToken cancel);

    /// Builds the EFI into request.OutDir\EFI, applies stage 2 and checks it with ocvalidate. A build that stops returns
    /// a result with Ok false and the reason; cancelling kills the builder's process tree and throws
    /// OperationCanceledException.
    Task<EfiBuildResult> BuildAsync(EfiBuildRequest request, IProgress<string> progress, CancellationToken cancel);
}
