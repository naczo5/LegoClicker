using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;
using LegoClickerCS.Core;

namespace LegoClickerCS;

public partial class HudWindow : Window
{
    private readonly DispatcherTimer _timer;
    private readonly Dictionary<string, TagVisual> _tagPool = new();
    private int _frameCounter;

    private sealed class TagVisual
    {
        public Rectangle Background { get; init; } = null!;
        public TextBlock Text { get; init; } = null!;
        public Rectangle HealthBar { get; init; } = null!;
        public double SmoothedX;
        public double SmoothedY;
        public bool HasInit;
        public int LastSeenFrame;
    }

    public HudWindow()
    {
        InitializeComponent();
        _frameCounter = 0;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        _timer.Tick += OnTick;
        _timer.Start();
    }

    private void OnTick(object? sender, EventArgs e)
    {
        // Nametags are rendered internally by bridge.dll (in-game), not via external WPF overlay.
        HideAllTags();
    }

    private static bool IsFinite(double v) => !(double.IsNaN(v) || double.IsInfinity(v));

    private bool ShouldRender(EntityInfo entity)
    {
        if (!IsFinite(entity.Sx) || !IsFinite(entity.Sy) || !IsFinite(entity.Dist)) return false;
        if (entity.Dist < 0.5 || entity.Dist > 72.0) return false;

        double w = ActualWidth > 0 ? ActualWidth : 1920;
        double h = ActualHeight > 0 ? ActualHeight : 1080;
        if (entity.Sx < -80 || entity.Sx > w + 80 || entity.Sy < -80 || entity.Sy > h + 80) return false;

        string name = SanitizeName(entity.Name);
        if (string.IsNullOrWhiteSpace(name)) return false;
        return true;
    }

    private static string SanitizeName(string raw)
    {
        if (string.IsNullOrEmpty(raw)) return string.Empty;
        char[] b = new char[raw.Length];
        int n = 0;
        for (int i = 0; i < raw.Length; i++)
        {
            if (raw[i] == '§' && i + 1 < raw.Length)
            {
                i++;
                continue;
            }
            b[n++] = raw[i];
        }
        return new string(b, 0, n).Trim();
    }

    private static string BuildEntityKey(EntityInfo entity, int index)
    {
        string baseName = SanitizeName(entity.Name);
        return $"{baseName}|{index}";
    }

    private void UpdateTag(string key, EntityInfo entity)
    {
        if (!_tagPool.TryGetValue(key, out var tag))
        {
            tag = new TagVisual
            {
                Background = new Rectangle
                {
                    RadiusX = 3,
                    RadiusY = 3,
                    Fill = new SolidColorBrush(Color.FromArgb(165, 0, 0, 0)),
                    Stroke = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255)),
                    StrokeThickness = 0.6
                },
                Text = new TextBlock
                {
                    Foreground = Brushes.White,
                    FontWeight = FontWeights.Bold,
                    Effect = new DropShadowEffect { BlurRadius = 5, ShadowDepth = 0, Color = Colors.Black, Opacity = 0.95 }
                },
                HealthBar = new Rectangle
                {
                    Height = 3
                }
            };

            OverlayCanvas.Children.Add(tag.Background);
            OverlayCanvas.Children.Add(tag.Text);
            OverlayCanvas.Children.Add(tag.HealthBar);
            _tagPool[key] = tag;
        }

        tag.LastSeenFrame = _frameCounter;
        tag.Background.Visibility = Visibility.Visible;
        tag.Text.Visibility = Visibility.Visible;
        tag.HealthBar.Visibility = Visibility.Visible;

        double scale = 2.8 / (entity.Dist + 0.5);
        if (scale < 0.65) scale = 0.65;
        if (scale > 1.35) scale = 1.35;

        tag.Text.Text = $"{SanitizeName(entity.Name)} {Math.Clamp(entity.Hp, 0, 40):0.#}HP";
        tag.Text.FontSize = 13.0 * scale;
        tag.Text.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

        double w = tag.Text.DesiredSize.Width;
        double h = tag.Text.DesiredSize.Height;
        double pad = 3.0 * scale;
        double x = entity.Sx - (w / 2) - pad;
        double y = entity.Sy - (h + 14.0 * scale);

        if (!tag.HasInit)
        {
            tag.SmoothedX = x;
            tag.SmoothedY = y;
            tag.HasInit = true;
        }
        else
        {
            tag.SmoothedX += (x - tag.SmoothedX) * 0.35;
            tag.SmoothedY += (y - tag.SmoothedY) * 0.35;
        }

        tag.Background.Width = w + pad * 2;
        tag.Background.Height = h + pad * 2;
        Canvas.SetLeft(tag.Background, tag.SmoothedX);
        Canvas.SetTop(tag.Background, tag.SmoothedY);

        Canvas.SetLeft(tag.Text, tag.SmoothedX + pad);
        Canvas.SetTop(tag.Text, tag.SmoothedY + pad);

        double hpPct = entity.Hp / 20.0;
        if (hpPct < 0) hpPct = 0;
        if (hpPct > 1) hpPct = 1;
        tag.HealthBar.Width = (w + pad * 2) * hpPct;
        Canvas.SetLeft(tag.HealthBar, tag.SmoothedX);
        Canvas.SetTop(tag.HealthBar, tag.SmoothedY + h + pad * 2 + 1);
        tag.HealthBar.Fill = new SolidColorBrush(Color.FromRgb((byte)(255 * (1 - hpPct)), (byte)(220 * hpPct + 35), 60));
    }

    private void HideAllTags()
    {
        foreach (var item in _tagPool.Values)
        {
            item.Background.Visibility = Visibility.Collapsed;
            item.Text.Visibility = Visibility.Collapsed;
            item.HealthBar.Visibility = Visibility.Collapsed;
        }
    }

    private void CleanupStaleTags()
    {
        if (_tagPool.Count == 0) return;

        List<string> remove = new();
        foreach (var kv in _tagPool)
        {
            if (_frameCounter - kv.Value.LastSeenFrame > 8)
            {
                kv.Value.Background.Visibility = Visibility.Collapsed;
                kv.Value.Text.Visibility = Visibility.Collapsed;
                kv.Value.HealthBar.Visibility = Visibility.Collapsed;
            }

            if (_frameCounter - kv.Value.LastSeenFrame > 240)
            {
                OverlayCanvas.Children.Remove(kv.Value.Background);
                OverlayCanvas.Children.Remove(kv.Value.Text);
                OverlayCanvas.Children.Remove(kv.Value.HealthBar);
                remove.Add(kv.Key);
            }
        }

        foreach (string key in remove)
        {
            _tagPool.Remove(key);
        }
    }
}
