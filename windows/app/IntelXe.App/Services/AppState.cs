// SPDX-License-Identifier: MIT
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXe.Core.Report;

namespace IntelXe.App;

/// What the pages share: this PC's facts (read once at start and on Refresh), the EFI planner, the last report.
public sealed class AppState
{
    public static AppState Current { get; } = new();

    public PcFacts? Facts { get; private set; }
    public IReadOnlyList<Check> Checks { get; private set; } = [];
    public bool Probing { get; private set; }
    public event Action? Changed;

    /// The seam for the shared EFI rules: PendingEfiPlanner until they are wired in.
    public IEfiPlanner Planner { get; set; } = new PendingEfiPlanner();

    public ReportResult? LastReport { get; set; }
    public ReportRunner Report { get; } = new();

    public async Task RefreshAsync()
    {
        if (Probing) return;
        Probing = true;
        Changed?.Invoke();
        try
        {
            var facts = await Task.Run(SystemProbe.Probe);
            Facts = facts;
            Checks = PcChecks.Evaluate(facts);
        }
        finally
        {
            Probing = false;
            Changed?.Invoke();
        }
    }
}
