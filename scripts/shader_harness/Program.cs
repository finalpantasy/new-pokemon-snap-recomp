// Offline Maxwell -> SPIR-V harness. Derived from Ryujinx.ShaderTools (MIT, Ryujinx Team and Contributors).
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace ShaderHarness
{
    class Program
    {
        private class GpuAccessor : IGpuAccessor
        {
            private readonly byte[] _data;
            private int _textures, _images;
            public GpuAccessor(byte[] data) { _data = data; }
            public SetBindingPair CreateConstantBufferBinding(int index) => new SetBindingPair(0, index + 1);
            public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _images; _images += count; return new SetBindingPair(3, b); }
            public SetBindingPair CreateStorageBufferBinding(int index) => new SetBindingPair(1, index);
            public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _textures; _textures += count; return new SetBindingPair(2, b); }
            public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_data)[(int)address..]);
            public GpuGraphicsState QueryGraphicsState() => new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles,
                TessSpacing.EqualSpacing, false, false, false, false, false, 1f, AlphaTestOp.Always, 0f, default, true, default, false, false, true /*originUpperLeft*/, false);
            public int QuerySamplerArrayLengthFromPool() => 32;
            public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
            public int QueryTextureArrayLengthFromPool() => 32;
        }


        // ---- NVN layer mode ----------------------------------------------------------------------------------
        // Binding spaces are per stage so one fixed VkPipelineLayout serves every pipeline (see docs/nvn-vulkan-design.md):
        //   set 0: binding 0 = Ryujinx support buffer, binding 1 + stage*16 + s = constant buffer c[s] of that stage
        //   set 1: storage buffers, binding stage*16 + ordinal
        //   set 2: combined image samplers, binding stage*32 + running counter
        //   set 3: storage images, binding stage*8 + running counter
        private class NvnAccessor : IGpuAccessor
        {
            private readonly byte[] _data; private readonly int _stage, _constOff, _codeOff;
            private int _textures, _images;
            public NvnAccessor(byte[] data, int stage, int constOff, int codeOff) { _data = data; _stage = stage; _constOff = constOff; _codeOff = codeOff; }
            public uint ConstantBuffer1Read(int offset)
            {
                int o = _constOff + offset;
                return (o >= 0 && o + 4 <= _data.Length) ? BitConverter.ToUInt32(_data, o) : 0;
            }
            public SetBindingPair CreateConstantBufferBinding(int index) => new SetBindingPair(0, 1 + _stage * 16 + index);
            public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _images; _images += count; return new SetBindingPair(3, _stage * 8 + b); }
            public SetBindingPair CreateStorageBufferBinding(int index) => new SetBindingPair(1, _stage * 16 + index);
            public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _textures; _textures += count; return new SetBindingPair(2, _stage * 32 + b); }
            // address 0 = start of the SPH (graphics) or of the code (compute), i.e. data[_codeOff..]
            public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_data)[(_codeOff + (int)address)..(_constOff & ~7)]);
            // NVN: window origin upper-left, depth near-is-zero (DepthMode=false means no [-1,1] -> [0,1] remap)
            public GpuGraphicsState QueryGraphicsState() => new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles,
                TessSpacing.EqualSpacing, false, false, false, false, false, 1f, AlphaTestOp.Always, 0f, default, true, default, false, false, true /*originUpperLeft*/, false);
            public int QuerySamplerArrayLengthFromPool() => 32;
            public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
            public int QueryTextureArrayLengthFromPool() => 32;
            // Host profile (bump the cache salt in nvn_shader.cpp when this changes): the SPIR-V must not require
            // capabilities that the layer's device does not enable. Fragment shader interlock is the one that the
            // translator declares by default and that no consumer GPU path here provides.
            public bool QueryHostSupportsFragmentShaderInterlock() => false;
            // Translator diagnostics ("instruction X is not implemented", "Invalid ...") are dropped by default. SNAP_XLATE_LOG=<file> in
            // --nvn-batch appends them as <program>, a tab and the message (audit: docs/nvn-vulkan-design.md, analysis/core/shader_audit.md).
            public void Log(string message) { if (LogW != null) lock (LogW) LogW.WriteLine(LogName + "\t" + message.Replace('\n', ' ')); }
        }
        static StreamWriter LogW; static string LogName = "";

        static string J(string s) => "\"" + s + "\"";

        // args: data.bin out.spv out.json stage(0 VS,1 FS,2 GS,5 CS) constOff codeOff
        // The translator always declares gl_PerVertex with ClipDistance[8] + CullDistance[1]; that sums to 9 and exceeds
        // maxCombinedClipAndCullDistances (8 on common GPUs), a validation error at pipeline creation. Shrink the clip array
        // to 7 entries (a fresh OpConstant, so the shared constant 8 used by other arrays is untouched) unless clip distance 7
        // is actually written.
        static byte[] FixClipCull(byte[] spv, int clipWritten)
        {
            if ((clipWritten & 0x80) != 0) { Console.Error.WriteLine("warning: clip distance 7 written; leaving gl_PerVertex as translated"); return spv; }
            uint[] w = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, w, 0, spv.Length);
            uint structId = 0;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xFFFF, len = w[i] >> 16;
                if (op == 72 && w[i + 3] == 11 && w[i + 4] == 3) structId = w[i + 1];   // OpMemberDecorate ... BuiltIn ClipDistance
                i += (int)len;
            }
            if (structId == 0) return spv;
            uint arrId = 0;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xFFFF, len = w[i] >> 16;
                if (op == 30 && w[i + 1] == structId) arrId = w[i + 4];                     // OpTypeStruct: member 2 (+2 for result id and 2 earlier members)
                i += (int)len;
            }
            if (arrId == 0) return spv;
            int arrPos = -1; uint lenId = 0;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xFFFF, len = w[i] >> 16;
                if (op == 28 && w[i + 1] == arrId) { arrPos = i; lenId = w[i + 3]; }
                i += (int)len;
            }
            if (arrPos < 0) return spv;
            int constEnd = -1; uint constType = 0;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xFFFF, len = w[i] >> 16;
                if (op == 43 && w[i + 2] == lenId) { constEnd = i + (int)len; constType = w[i + 1]; }
                i += (int)len;
            }
            if (constEnd < 0) return spv;
            uint newId = w[3]; w[3] = newId + 1;
            w[arrPos + 3] = newId;
            var o = new List<uint>(w.Length + 4);
            o.AddRange(w.Take(constEnd));
            o.Add((4u << 16) | 43u); o.Add(constType); o.Add(newId); o.Add(7);
            o.AddRange(w.Skip(constEnd));
            // the array type may precede the constant (it cannot: its length operand must be defined first), so positions after constEnd shifted only
            byte[] r = new byte[o.Count * 4];
            Buffer.BlockCopy(o.ToArray(), 0, r, 0, r.Length);
            return r;
        }

        static int Nvn(string[] a)
        {
            return NvnTranslate(File.ReadAllBytes(a[0]), a[1], a[2], int.Parse(a[3]), int.Parse(a[4]), int.Parse(a[5]));
        }

        // --nvn-batch list.tsv binsDir outDir salt: every program of the list (corpus from scripts/xlate_corpus.py) is translated in
        // this process and written as <key>.spv/.json, key = SHA-256(data section || salt|stage|codeOff), the NVN layer's cache key
        // (runtime/nvn/nvn_shader.cpp Sha256Hex). Failures are listed in outDir\failures.tsv and do not stop the run.
        // Optional 5th/6th arguments first and count translate a slice of the list (chunks under 10 minutes).
        static int NvnBatch(string[] a)
        {
            string[] lines = File.ReadAllLines(a[0]);
            string binsDir = a[1], outDir = a[2], salt = a[3];
            int first = a.Length > 4 ? int.Parse(a[4]) : 0, count = a.Length > 5 ? int.Parse(a[5]) : lines.Length;
            Directory.CreateDirectory(outDir);
            using var fail = new StreamWriter(Path.Combine(outDir, "failures" + first + ".tsv"));
            int ok = 0, bad = 0;
            string logPath = Environment.GetEnvironmentVariable("SNAP_XLATE_LOG");
            if (!string.IsNullOrEmpty(logPath)) LogW = new StreamWriter(logPath, true);
            for (int i = first; i < Math.Min(lines.Length, first + count); i++)
            {
                string[] f = lines[i].Split('	');
                byte[] data = File.ReadAllBytes(Path.Combine(binsDir, f[0] + ".bin"));
                int stage = int.Parse(f[1]), constOff = int.Parse(f[2]), codeOff = int.Parse(f[3]);
                LogName = f[4];
                byte[] extra = System.Text.Encoding.ASCII.GetBytes(salt + "|" + stage + "|" + codeOff);
                var sha = System.Security.Cryptography.SHA256.Create();
                sha.TransformBlock(data, 0, data.Length, null, 0);
                sha.TransformFinalBlock(extra, 0, extra.Length);
                string key = Convert.ToHexString(sha.Hash).ToLowerInvariant();
                try { NvnTranslate(data, Path.Combine(outDir, key + ".spv"), Path.Combine(outDir, key + ".json"), stage, constOff, codeOff); ok++; }
                catch (Exception e) { bad++; fail.WriteLine(f[0] + "\t" + key + "\t" + f[4] + "\t" + e.GetType().Name + ": " + e.Message.Replace('\n', ' ')); }
            }
            LogW?.Flush();
            Console.WriteLine("ok " + ok + " failed " + bad);
            return bad == 0 ? 0 : 1;
        }

        static int NvnTranslate(byte[] data, string outSpv, string outJson, int stage, int constOff, int codeOff)
        {
            TranslationFlags flags = TranslationFlags.DebugMode;
            if (stage == 5) flags |= TranslationFlags.Compute;
            var ctx = Translator.CreateContext(0, new NvnAccessor(data, stage, constOff, codeOff), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
            ShaderProgram p = ctx.Translate();
            AtomicWrite(outSpv, FixClipCull(p.BinaryCode, (int)p.Info.ClipDistancesWritten));   // .tmp then rename; the json goes last (a kill mid-write must not leave a half entry)
            var sb = new System.Text.StringBuilder();
            sb.Append("{\"stage\":" + stage + ",\"usesInstanceId\":" + (p.Info.UsesInstanceId ? 1 : 0) + ",\"usesFragCoord\":" + (p.Info.UsesFragCoord ? 1 : 0)
                + ",\"clipDistances\":" + p.Info.ClipDistancesWritten + ",\"fragmentOutputMap\":" + p.Info.FragmentOutputMap);
            sb.Append(",\"cbuffers\":[");
            bool first = true;
            foreach (var c in p.Info.CBuffers) { sb.Append((first ? "" : ",") + "{\"slot\":" + c.Slot + ",\"set\":" + c.Set + ",\"binding\":" + c.Binding + "}"); first = false; }
            sb.Append("],\"sbuffers\":[");
            first = true;
            foreach (var c in p.Info.SBuffers) { sb.Append((first ? "" : ",") + "{\"slot\":" + c.Slot + ",\"set\":" + c.Set + ",\"binding\":" + c.Binding + ",\"sbCbSlot\":" + c.SbCbSlot + ",\"sbCbOffset\":" + c.SbCbOffset + ",\"write\":" + (((int)c.Flags & 1) != 0 ? 1 : 0) + "}"); first = false; }
            sb.Append("],\"textures\":[");
            first = true;
            foreach (var t in p.Info.Textures) { sb.Append((first ? "" : ",") + "{\"set\":" + t.Set + ",\"binding\":" + t.Binding + ",\"handleIndex\":" + t.HandleIndex + ",\"cbufSlot\":" + t.CbufSlot + ",\"type\":" + (int)t.Type + ",\"array\":" + t.ArrayLength + ",\"separate\":" + (t.Separate ? 1 : 0) + "}"); first = false; }
            sb.Append("],\"images\":[");
            first = true;
            foreach (var t in p.Info.Images) { sb.Append((first ? "" : ",") + "{\"set\":" + t.Set + ",\"binding\":" + t.Binding + ",\"handleIndex\":" + t.HandleIndex + ",\"cbufSlot\":" + t.CbufSlot + ",\"type\":" + (int)t.Type + ",\"array\":" + t.ArrayLength + ",\"flags\":" + (int)t.Flags + "}"); first = false; }
            sb.Append("]}");
            AtomicWrite(outJson, System.Text.Encoding.UTF8.GetBytes(sb.ToString()));
            return 0;
        }

        static void AtomicWrite(string path, byte[] bytes)
        {
            string tmp = path + "." + Environment.ProcessId + ".wtmp";
            File.WriteAllBytes(tmp, bytes);
            File.Move(tmp, path, true);
        }

        static int Batch(string list, string outDir)
        {
            Directory.CreateDirectory(outDir);
            using var res = new StreamWriter(Path.Combine(outDir, "results.tsv"));
            foreach (string line in File.ReadAllLines(list))
            {
                string[] f = line.Split('\t');
                string name = Path.GetFileNameWithoutExtension(f[0]);
                var sw = System.Diagnostics.Stopwatch.StartNew();
                try
                {
                    TranslationFlags flags = TranslationFlags.DebugMode;
                    if (f[1] == "CS") flags |= TranslationFlags.Compute;
                    var ctx = Translator.CreateContext(0, new GpuAccessor(File.ReadAllBytes(f[0])), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    ShaderProgram p = ctx.Translate();
                    File.WriteAllBytes(Path.Combine(outDir, name + ".spv"), p.BinaryCode);
                    res.WriteLine(name + "\tok\t" + p.BinaryCode.Length + "\t" + p.Info.CBuffers.Count + "\t" + p.Info.Textures.Count + "\t" + p.Info.SBuffers.Count + "\t" + sw.ElapsedMilliseconds);
                }
                catch (Exception e)
                {
                    string m = e.GetType().Name + ": " + e.Message.Replace('\n', ' ');
                    string at = e.StackTrace == null ? "" : e.StackTrace.Split('\n')[0].Trim();
                    res.WriteLine(name + "	FAIL	" + m + " @ " + at);
                }
                res.Flush();
            }
            return 0;
        }

        static int Main(string[] args)
        {
            // args: in.bin out.spv [compute]   |   --batch list.tsv outdir  (list: path<TAB>stage; writes results.tsv)
            if (args.Length == 7 && args[0] == "--nvn") return Nvn(args[1..]);
            if (args.Length >= 5 && args[0] == "--nvn-batch") return NvnBatch(args[1..]);
            if (args.Length == 3 && args[0] == "--batch") return Batch(args[1], args[2]);
            if (args.Length < 2) { Console.Error.WriteLine("usage: in.bin out.spv [compute]"); return 2; }
            TranslationFlags flags = TranslationFlags.DebugMode;
            if (args.Length > 2 && args[2] == "compute") flags |= TranslationFlags.Compute;
            byte[] data = File.ReadAllBytes(args[0]);
            var opts = new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags);
            var ctx = Translator.CreateContext(0, new GpuAccessor(data), opts);
            ShaderProgram program = ctx.Translate();
            if (program.BinaryCode != null) File.WriteAllBytes(args[1], program.BinaryCode);
            else File.WriteAllText(args[1], program.Code);
            Console.WriteLine("ok stage=" + program.Info.Stage + " cb=" + program.Info.CBuffers.Count + " tex=" + program.Info.Textures.Count + " ssbo=" + program.Info.SBuffers.Count + " spv=" + (program.BinaryCode?.Length ?? 0));
            if (args.Length > 3 && args[3] == "info")
            {
                foreach (var t in program.Info.Textures) Console.WriteLine("  tex set=" + t.Set + " bind=" + t.Binding + " cbufSlot=" + t.CbufSlot + " handleIdx=" + t.HandleIndex + " type=" + t.Type + " fmt=" + t.Format + " flags=" + t.Flags);
                foreach (var c in program.Info.CBuffers) Console.WriteLine("  cbuf set=" + c.Set + " bind=" + c.Binding + " slot=" + c.Slot);
                foreach (var c in program.Info.SBuffers) Console.WriteLine("  sbuf set=" + c.Set + " bind=" + c.Binding + " slot=" + c.Slot + " sbCbSlot=" + c.SbCbSlot + " sbCbOff=" + c.SbCbOffset);
            }
            return 0;
        }
    }
}
