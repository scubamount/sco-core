// Program.cs: the C# SDK's test. Exit 0 = every check passed.
//
//   dotnet run -c Release --project sdk/csharp/Sco.Sdk.Tests [-- tests/abi_v1.c tests/abi_storage.c ...]
//
// 1. Every pin of Pins.cs holds for the C# structs (Layout.cs).
// 2. Given abi_*.c files, Pins.cs equals their SIZE / AT / PIN lines, line for line.
// 3. The wrappers behave against a stand-in host table (FakeHost.cs).
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Sco.Sdk.Tests
{
    internal static class Program
    {
        private static int Main(string[] args)
        {
            int failures = 0;
            foreach ((string file, string line) in Pins.Lines)
            {
                string? why = Layout.Check(line);
                if (why == null) continue;
                Console.WriteLine($"FAIL {file}: {line}  ({why})");
                ++failures;
            }
            Console.WriteLine($"layout: {Pins.Lines.Length} pins checked, {failures} failed");

            foreach (string path in args)
            {
                string name = "tests/" + Path.GetFileName(path);
                List<string> c = File.ReadAllLines(path).Where(Layout.IsPinLine).ToList();
                List<string> copy = Pins.Lines.Where(p => p.File == name).Select(p => p.Line).ToList();
                if (copy.Count == 0)
                {
                    Console.WriteLine($"FAIL {name}: Pins.cs has no lines for it");
                    ++failures;
                    continue;
                }
                int n = Math.Max(c.Count, copy.Count);
                for (int i = 0; i < n; ++i)
                {
                    string? a = i < c.Count ? c[i] : null, b = i < copy.Count ? copy[i] : null;
                    if (a == b) continue;
                    Console.WriteLine($"FAIL {name}: pin {i + 1} is \"{a}\" in C, \"{b}\" in Pins.cs; copy the C lines");
                    ++failures;
                    break;
                }
                Console.WriteLine($"pins: {name} {c.Count} lines");
            }

            failures += FakeHost.Run();
            Console.WriteLine(failures == 0 ? "Sco.Sdk.Tests: OK" : $"Sco.Sdk.Tests: FAILED ({failures})");
            return failures == 0 ? 0 : 1;
        }
    }
}
