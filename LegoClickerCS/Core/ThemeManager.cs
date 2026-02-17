using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace LegoClickerCS.Core;

public static class ThemeManager
{
    public static string CurrentTheme { get; private set; } = "Dark";
    
    public static event Action? ThemeChanged;
    
    public static readonly Dictionary<string, ThemeColors> Themes = new()
    {
        ["Dark"] = new ThemeColors
        {
            Background = Color.FromRgb(30, 30, 35),
            Panel = Color.FromRgb(40, 40, 48),
            SliderBg = Color.FromRgb(60, 60, 70),
            SliderFg = Color.FromRgb(120, 120, 130),
            Accent = Color.FromRgb(0, 220, 180),
            Text = Color.FromRgb(220, 220, 220),
            DimText = Color.FromRgb(150, 150, 150)
        },
        ["Purple"] = new ThemeColors
        {
            Background = Color.FromRgb(26, 21, 37),
            Panel = Color.FromRgb(35, 28, 50),
            SliderBg = Color.FromRgb(55, 45, 75),
            SliderFg = Color.FromRgb(120, 100, 150),
            Accent = Color.FromRgb(155, 89, 182),
            Text = Color.FromRgb(220, 210, 230),
            DimText = Color.FromRgb(140, 130, 160)
        },
        ["Red"] = new ThemeColors
        {
            Background = Color.FromRgb(31, 21, 21),
            Panel = Color.FromRgb(45, 30, 30),
            SliderBg = Color.FromRgb(70, 50, 50),
            SliderFg = Color.FromRgb(140, 100, 100),
            Accent = Color.FromRgb(231, 76, 60),
            Text = Color.FromRgb(230, 210, 210),
            DimText = Color.FromRgb(160, 130, 130)
        },
        ["Blue"] = new ThemeColors
        {
            Background = Color.FromRgb(21, 26, 31),
            Panel = Color.FromRgb(30, 38, 48),
            SliderBg = Color.FromRgb(50, 60, 75),
            SliderFg = Color.FromRgb(100, 120, 150),
            Accent = Color.FromRgb(52, 152, 219),
            Text = Color.FromRgb(210, 220, 230),
            DimText = Color.FromRgb(130, 145, 165)
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
