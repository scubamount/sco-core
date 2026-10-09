// Commands.cs: CommandBuilder, Args, Reply and the command trampoline.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>A command handler: runs on the game thread, reads args, writes reply.</summary>
    public delegate ScoResult CommandHandler(Args args, Reply reply);

    /// <summary>The arguments of one command call, valid only during it. The host has already
    /// checked the count and types against the definition; a wrong index or type returns the
    /// fallback (0, 0.0, "", false), never throws.</summary>
    public readonly unsafe ref struct Args
    {
        private readonly ScoArg* _args;
        private readonly uint _n;

        internal Args(ScoArg* args, uint n) { _args = args; _n = args == null ? 0 : n; }

        public int Count => (int)_n;

        public ScoArgType Type(int i) => (uint)i < _n ? (ScoArgType)_args[i].type : ScoArgType.Force32;

        public long Int(int i) => Type(i) == ScoArgType.Int ? _args[i].v.i : 0;
        public double Float(int i) => Type(i) == ScoArgType.Float ? _args[i].v.f : 0.0;
        public string String(int i) => Type(i) == ScoArgType.String ? Utf8.Read(_args[i].v.s) ?? "" : "";
        public bool Bool(int i) => Type(i) == ScoArgType.Bool && _args[i].v.i == 1;
    }

    /// <summary>The reply buffer of one command call (256 bytes with the NUL on the real host).</summary>
    public readonly unsafe ref struct Reply
    {
        private readonly byte* _buf;
        private readonly uint _size;

        internal Reply(byte* buf, uint size) { _buf = buf; _size = size; }

        /// <summary>Bytes available, the NUL included.</summary>
        public int Capacity => (int)_size;

        /// <summary>Writes text, cut at the buffer on a whole UTF-8 character.</summary>
        public void Set(string text) => Utf8.Write(text ?? "", _buf, _size);
    }

    /// <summary>Collects one command and registers it: Title, Help, Capability, Arg..., Handle,
    /// Register. The title defaults to the name. Strings live in the plugin's native arena until
    /// it unloads; the handler sits in the registry under an id.</summary>
    public sealed unsafe class CommandBuilder
    {
        private readonly Plugin _plugin;
        private readonly string _name;
        private string? _title, _help, _capability;
        private readonly List<(string name, ScoArgType type, string? help)> _args = new();
        private CommandHandler? _fn;

        public CommandBuilder(Plugin plugin, string name)
        {
            _plugin = plugin ?? throw new ArgumentNullException(nameof(plugin));
            _name = name ?? throw new ArgumentNullException(nameof(name));
        }

        public CommandBuilder Title(string title) { _title = title; return this; }
        public CommandBuilder Help(string help) { _help = help; return this; }
        public CommandBuilder Capability(string capability) { _capability = capability; return this; }

        /// <summary>Adds an argument (at most 16); its type is what Args reads.</summary>
        public CommandBuilder Arg(string name, ScoArgType type, string? help = null)
        {
            _args.Add((name, type, help));
            return this;
        }

        public CommandBuilder Handle(CommandHandler fn) { _fn = fn; return this; }

        /// <summary>The host's answer: Ok, or BadArg for a bad, duplicate or foreign name, a string
        /// too long, or no handler.</summary>
        public ScoResult Register()
        {
            ScoApi* api = _plugin.Api;
            if (_fn == null || api == null || !_plugin.IsLoaded) return ScoResult.BadArg;
            NativeArena arena = _plugin.Arena;
            ScoArgDef* defs = null;
            if (_args.Count > 0)
            {
                defs = (ScoArgDef*)arena.Alloc((nuint)(_args.Count * sizeof(ScoArgDef)));
                for (int i = 0; i < _args.Count; ++i)
                {
                    defs[i].name = arena.Add(_args[i].name);
                    defs[i].type = (uint)_args[i].type;
                    defs[i].help = arena.Add(_args[i].help);
                }
            }
            var entry = new CommandEntry(_plugin, _name, _fn);
            nint id = Registry.Add(entry);
            ScoCommand cmd = default;
            cmd.size = (uint)sizeof(ScoCommand);
            cmd.name = arena.Add(_name);
            cmd.title = arena.Add(_title ?? _name);
            cmd.help = arena.Add(_help);
            cmd.capability = arena.Add(_capability);
            cmd.args = defs;
            cmd.nargs = (uint)_args.Count;
            cmd.arg_def_size = (uint)sizeof(ScoArgDef);
            cmd.fn = &CommandTrampoline;
            cmd.ctx = (void*)id;
            ScoResult r = api->register_command(_plugin.SelfPtr, &cmd);
            if (r != ScoResult.Ok) Registry.Remove(id);
            return r;
        }

        private sealed class CommandEntry : Entry
        {
            public CommandEntry(Plugin p, string name, CommandHandler fn) : base(p) { Name = name; Fn = fn; }
            public string Name { get; }
            public CommandHandler Fn { get; }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult CommandTrampoline(ScoArg* args, uint nargs, void* ctx, byte* reply, uint replySize)
        {
            try
            {
                CommandEntry? e = Registry.Get<CommandEntry>(ctx);
                if (e == null) return ScoResult.NotFound;
                try { return e.Fn(new Args(args, nargs), new Reply(reply, replySize)); }
                catch (Exception ex)
                {
                    e.Owner.ReportException(e.Name, ex);
                    Utf8.Write($"{e.Name} failed: {ex.Message}", reply, replySize);
                    return ScoResult.Failed;
                }
            }
            catch { return ScoResult.Failed; }
        }
    }
}
