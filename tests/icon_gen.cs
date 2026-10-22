using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class SCIcon {
    static GraphicsPath RoundRect(float x, float y, float w, float h, float r) {
        var p = new GraphicsPath();
        float d = Math.Min(r * 2, Math.Min(w, h));
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static GraphicsPath Star(float cx, float cy, float outer, float inner) {
        var pts = new PointF[8];
        for (int i = 0; i < 8; i++) {
            double a = -Math.PI / 2 + i * Math.PI / 4;
            float r = (i % 2 == 0) ? outer : inner;
            pts[i] = new PointF((float)(cx + Math.Cos(a) * r), (float)(cy + Math.Sin(a) * r));
        }
        var p = new GraphicsPath();
        p.AddPolygon(pts);
        return p;
    }

    public static Bitmap MakeBitmap(int S) {
        var bmp = new Bitmap(S, S, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.CompositingQuality = CompositingQuality.HighQuality;

            float pad = S * 0.012f;
            float size = S - pad * 2f;
            float radius = S * 0.225f;

            using (var tile = RoundRect(pad, pad, size, size, radius)) {
                using (var brush = new LinearGradientBrush(
                        new PointF(pad, pad), new PointF(pad + size, pad + size),
                        Color.FromArgb(255, 96, 160, 255), Color.FromArgb(255, 27, 70, 205))) {
                    var blend = new ColorBlend(3);
                    blend.Colors = new[] {
                        Color.FromArgb(255, 96, 160, 255),
                        Color.FromArgb(255, 48, 110, 240),
                        Color.FromArgb(255, 27, 70, 205)
                    };
                    blend.Positions = new[] { 0f, 0.55f, 1f };
                    brush.InterpolationColors = blend;
                    g.FillPath(brush, tile);
                }

                g.SetClip(tile);
                using (var gloss = new LinearGradientBrush(
                        new PointF(0, 0), new PointF(0, pad + size * 0.58f),
                        Color.FromArgb(48, 255, 255, 255), Color.FromArgb(0, 255, 255, 255))) {
                    g.FillRectangle(gloss, 0, 0, S, S * 0.58f);
                }
                g.ResetClip();

                using (var inner = RoundRect(pad + S * 0.006f, pad + S * 0.006f,
                        size - S * 0.012f, size - S * 0.012f, radius - S * 0.005f))
                using (var pen = new Pen(Color.FromArgb(40, 255, 255, 255), Math.Max(1f, S * 0.008f))) {
                    g.DrawPath(pen, inner);
                }
            }

            using (var white = new SolidBrush(Color.FromArgb(255, 255, 255, 255))) {
                // handle
                using (var p = RoundRect(S * 0.405f, S * 0.255f, S * 0.19f, S * 0.075f, S * 0.038f))
                    g.FillPath(white, p);
                // lid
                using (var p = RoundRect(S * 0.255f, S * 0.335f, S * 0.49f, S * 0.085f, S * 0.042f))
                    g.FillPath(white, p);

                // body: wider at top, rounded bottom
                float topY = S * 0.445f;
                float botY = S * 0.785f;
                float topL = S * 0.300f;
                float topR = S * 0.700f;
                float botL = S * 0.325f;
                float botR = S * 0.675f;
                float br = S * 0.055f;
                using (var body = new GraphicsPath()) {
                    body.AddLine(topL, topY, topR, topY);
                    body.AddLine(topR, topY, botR, botY - br);
                    body.AddArc(botR - 2 * br, botY - 2 * br, 2 * br, 2 * br, 0, 90);
                    body.AddLine(botR - br, botY, botL + br, botY);
                    body.AddArc(botL, botY - 2 * br, 2 * br, 2 * br, 90, 90);
                    body.AddLine(botL, botY - br, topL, topY);
                    body.CloseFigure();
                    g.FillPath(white, body);
                }
            }

            using (var slot = new SolidBrush(Color.FromArgb(255, 46, 103, 224))) {
                using (var p = RoundRect(S * 0.407f, S * 0.520f, S * 0.042f, S * 0.190f, S * 0.021f))
                    g.FillPath(slot, p);
                using (var p = RoundRect(S * 0.551f, S * 0.520f, S * 0.042f, S * 0.190f, S * 0.021f))
                    g.FillPath(slot, p);
            }

            using (var spark = new SolidBrush(Color.FromArgb(242, 255, 255, 255)))
            using (var st = Star(S * 0.775f, S * 0.210f, S * 0.098f, S * 0.026f))
                g.FillPath(spark, st);
            using (var dot = new SolidBrush(Color.FromArgb(200, 255, 255, 255)))
                g.FillEllipse(dot, S * 0.860f, S * 0.315f, S * 0.036f, S * 0.036f);
        }
        return bmp;
    }

    public static void SavePng(string path, int size) {
        using (var bmp = MakeBitmap(size))
            bmp.Save(path, ImageFormat.Png);
    }

    static byte[] PngBytes(Bitmap bmp) {
        using (var ms = new MemoryStream()) {
            bmp.Save(ms, ImageFormat.Png);
            return ms.ToArray();
        }
    }

    static byte[] DibBytes(Bitmap bmp) {
        int w = bmp.Width;
        int h = bmp.Height;
        using (var ms = new MemoryStream()) {
            using (var bw = new BinaryWriter(ms)) {
                bw.Write(40);
                bw.Write(w);
                bw.Write(h * 2);
                bw.Write((short)1);
                bw.Write((short)32);
                bw.Write(0);
                bw.Write(w * h * 4);
                bw.Write(0);
                bw.Write(0);
                bw.Write(0);
                bw.Write(0);

                var bd = bmp.LockBits(new Rectangle(0, 0, w, h),
                    ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                try {
                    byte[] row = new byte[bd.Stride];
                    for (int y = h - 1; y >= 0; y--) {
                        Marshal.Copy(IntPtr.Add(bd.Scan0, y * bd.Stride), row, 0, bd.Stride);
                        bw.Write(row, 0, w * 4);
                    }
                }
                finally {
                    bmp.UnlockBits(bd);
                }

                int maskStride = ((w + 31) / 32) * 4;
                bw.Write(new byte[maskStride * h]);
            }
            return ms.ToArray();
        }
    }

    public static void SaveIco(string path, int[] sizes) {
        var frames = new List<byte[]>();
        foreach (int s in sizes) {
            using (var bmp = MakeBitmap(s))
                frames.Add(s >= 256 ? PngBytes(bmp) : DibBytes(bmp));
        }

        using (var fs = File.Create(path))
        using (var bw = new BinaryWriter(fs)) {
            bw.Write((short)0);
            bw.Write((short)1);
            bw.Write((short)sizes.Length);

            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++) {
                int s = sizes[i];
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)0);
                bw.Write((byte)0);
                bw.Write((short)1);
                bw.Write((short)32);
                bw.Write(frames[i].Length);
                bw.Write(offset);
                offset += frames[i].Length;
            }
            foreach (var f in frames) bw.Write(f);
        }
    }

    public static void Main(string[] args) {
        if (args.Length < 3) {
            Console.WriteLine("usage: icon_gen <ico> <smallIco> <previewDir>");
            return;
        }
        var sizes = new[] { 16, 24, 32, 48, 64, 128, 256 };
        SaveIco(args[0], sizes);
        SaveIco(args[1], sizes);
        SavePng(Path.Combine(args[2], "_new_icon_preview.png"), 512);
        SavePng(Path.Combine(args[2], "_new_icon_32.png"), 32);
        Console.WriteLine("icons written");
    }
}
