// SPDX-License-Identifier: MIT
// Starting the builder's Python: without a console window, its output read line by line as UTF-8, and on cancel the
// whole process tree killed (OpCore Simplify starts iasl, macserial and ocvalidate). An interface so that the tests run
// the builder's logic against a fake Python.
using System.Diagnostics;
using System.Text;

namespace IntelXe.Core.EfiBuild;

public interface IProcessRunner
{
    /// Runs the process to its end and returns its exit code; each line of its standard output and error goes to the
    /// callbacks as it comes. Cancelling kills the process tree and throws OperationCanceledException.
    Task<int> RunAsync(ProcessSpec spec, Action<string> stdout, Action<string> stderr, CancellationToken cancel);
}

public sealed class SystemProcessRunner : IProcessRunner
{
    public async Task<int> RunAsync(ProcessSpec spec, Action<string> stdout, Action<string> stderr, CancellationToken cancel)
    {
        cancel.ThrowIfCancellationRequested();
        var psi = new ProcessStartInfo(spec.FileName)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
            WorkingDirectory = spec.WorkingDirectory,
        };
        foreach (var a in spec.Args) psi.ArgumentList.Add(a);
        using var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var outDone = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var errDone = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        p.OutputDataReceived += (_, e) => { if (e.Data is null) outDone.TrySetResult(); else stdout(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data is null) errDone.TrySetResult(); else stderr(e.Data); };
        if (!p.Start()) throw new EfiBuildException($"{spec.FileName} did not start");
        // nothing is ever typed: a question the builder did not expect reads end of input and stops it
        p.StandardInput.Close();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        try
        {
            await p.WaitForExitAsync(cancel);
        }
        catch (OperationCanceledException)
        {
            try { p.Kill(entireProcessTree: true); } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException) { }
            try { await p.WaitForExitAsync(CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(10), CancellationToken.None); } catch (TimeoutException) { }
            throw;
        }
        await Task.WhenAll(outDone.Task, errDone.Task).WaitAsync(TimeSpan.FromSeconds(10), CancellationToken.None).ContinueWith(_ => { }, TaskScheduler.Default);
        return p.ExitCode;
    }
}
