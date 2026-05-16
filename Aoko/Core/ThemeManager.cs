using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace Aoko.Core;

public static class ThemeManager
{
    public static string CurrentTheme { get; private set; } = "Slate";

    public static event Action? ThemeChanged;

    public static readonly Dictionary<string, ThemeColors> Themes = new()
    {
        ["Slate"] = new ThemeColors
        {
            Background = Color.FromRgb(0x0A, 0x0B, 0x0F),
            Panel = Color.FromRgb(0x12, 0x14, 0x1A),
            SliderBg = Color.FromRgb(0x18, 0x1B, 0x22),
            SliderFg = Color.FromRgb(0x2A, 0x2F, 0x38),
            Accent = Color.FromRgb(0xC7, 0x62, 0x5A),
            Text = Color.FromRgb(0xE8, 0xEA, 0xEE),
            DimText = Color.FromRgb(0x7A, 0x82, 0x90)
        },
        ["Ink"] = new ThemeColors
        {
            Background = Color.FromRgb(0x08, 0x09, 0x0B),
            Panel = Color.FromRgb(0x10, 0x11, 0x15),
            SliderBg = Color.FromRgb(0x16, 0x18, 0x1C),
            SliderFg = Color.FromRgb(0x26, 0x28, 0x30),
            Accent = Color.FromRgb(0xB0, 0xB6, 0xC0),
            Text = Color.FromRgb(0xE8, 0xEA, 0xEE),
            DimText = Color.FromRgb(0x7A, 0x82, 0x8F)
        },
        ["Graphite"] = new ThemeColors
        {
            Background = Color.FromRgb(0x0B, 0x0B, 0x0D),
            Panel = Color.FromRgb(0x13, 0x13, 0x16),
            SliderBg = Color.FromRgb(0x19, 0x19, 0x1C),
            SliderFg = Color.FromRgb(0x2A, 0x2A, 0x2D),
            Accent = Color.FromRgb(0xB8, 0x9B, 0x82),
            Text = Color.FromRgb(0xE8, 0xE8, 0xEA),
            DimText = Color.FromRgb(0x82, 0x82, 0x7E)
        },
        ["Steel"] = new ThemeColors
        {
            Background = Color.FromRgb(0x08, 0x09, 0x0C),
            Panel = Color.FromRgb(0x0F, 0x12, 0x18),
            SliderBg = Color.FromRgb(0x16, 0x1A, 0x21),
            SliderFg = Color.FromRgb(0x26, 0x2C, 0x35),
            Accent = Color.FromRgb(0x6B, 0x8D, 0xAB),
            Text = Color.FromRgb(0xE5, 0xE8, 0xEE),
            DimText = Color.FromRgb(0x72, 0x86, 0xA0)
        }
    };

    public static List<string> GetThemeNames() => new(Themes.Keys);

    public static void ApplyTheme(string themeName)
    {
        if (!Themes.TryGetValue(themeName, out var colors))
            return;

        CurrentTheme = themeName;

        var app = Application.Current;
        if (app == null) return;

        app.Resources["BgColor"] = colors.Background;
        app.Resources["PanelColor"] = colors.Panel;
        app.Resources["SliderBgColor"] = colors.SliderBg;
        app.Resources["SliderFgColor"] = colors.SliderFg;
        app.Resources["AccentColor"] = colors.Accent;
        app.Resources["TextColor"] = colors.Text;
        app.Resources["DimTextColor"] = colors.DimText;

        app.Resources["BgBrush"] = new SolidColorBrush(colors.Background);
        app.Resources["PanelBrush"] = new SolidColorBrush(colors.Panel);
        app.Resources["SliderBgBrush"] = new SolidColorBrush(colors.SliderBg);
        app.Resources["SliderFgBrush"] = new SolidColorBrush(colors.SliderFg);
        app.Resources["AccentBrush"] = new SolidColorBrush(colors.Accent);
        app.Resources["TextBrush"] = new SolidColorBrush(colors.Text);
        app.Resources["DimTextBrush"] = new SolidColorBrush(colors.DimText);

        ThemeChanged?.Invoke();
    }
}

public class ThemeColors
{
    public Color Background { get; set; }
    public Color Panel { get; set; }
    public Color SliderBg { get; set; }
    public Color SliderFg { get; set; }
    public Color Accent { get; set; }
    public Color Text { get; set; }
    public Color DimText { get; set; }
}
