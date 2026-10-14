using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;

namespace NvofControl
{
    public static class ResolutionSettings
    {
        public static readonly int[] Heights={0,480,720,1080,1440,2160,4320};
        public static bool Valid(int value) { return (value>=-5 && value<=0) || Array.IndexOf(SettingsStore.OutputLimits,value)>=0; }
        public static int[] Parse(string text)
        {
            string[] parts=text.Split(',');int[] result=new int[7];
            if(parts.Length!=7)return result;
            for(int i=0;i<7;++i) {
                string token=parts[i].Trim(' ','\t');
                if(token.StartsWith("+",StringComparison.Ordinal) || !Int32.TryParse(token,NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture,out result[i]) || !Valid(result[i]))return new int[7];
            }
            return result;
        }
        public static string Serialize(int[] values)
        {
            if(values==null || values.Length!=7)throw new ArgumentException("Seven resolution rules are required.");
            string[] parts=new string[7];
            for(int i=0;i<7;++i) {if(!Valid(values[i]))throw new ArgumentOutOfRangeException("values");parts[i]=values[i].ToString(CultureInfo.InvariantCulture);}
            return String.Join(",",parts);
        }
        public static string Label(int value) { return value==0?"기본 설정 사용":value==-1?"보간 안 함":value<0?"원본 ×"+(-value):"최대 "+value+" fps"; }
        public static string BandLabel(int i) {return i==0?"480p 미만":Heights[i]+"p 이상";}
        public static string RangeLabel(int i) {return i==6?"4,320 px 이상":(i==0?"1":Heights[i].ToString("N0",CultureInfo.InvariantCulture))+"–"+(Heights[i+1]-1).ToString("N0",CultureInfo.InvariantCulture)+" px";}
        public static void Fill(ComboBox combo,int selected,bool inherit)
        {
            if(inherit)combo.Items.Add(new ComboBoxItem{Content=Label(0),Tag=0});
            foreach(int cap in SettingsStore.OutputLimits)combo.Items.Add(new ComboBoxItem{Content=Label(cap),Tag=cap});
            for(int m=2;m<=5;++m)combo.Items.Add(new ComboBoxItem{Content=Label(-m),Tag=-m});
            if(inherit)combo.Items.Add(new ComboBoxItem{Content=Label(-1),Tag=-1});
            foreach(ComboBoxItem item in combo.Items)if((int)item.Tag==selected)combo.SelectedItem=item;
        }
    }

    public sealed class ResolutionEditor
    {
        public readonly Window Window;
        public readonly ComboBox[] Choices=new ComboBox[7];
        public readonly ComboBox Common;
        private readonly Action<int[],int> saveDraft;
        public void SaveDraft() {saveDraft(Rules,CommonRule);}
        public int[] Rules {get {int[] values=new int[7];for(int i=0;i<7;++i)values[i]=(int)((ComboBoxItem)Choices[i].SelectedItem).Tag;return values;}}
        public int CommonRule {get {return (int)((ComboBoxItem)Common.SelectedItem).Tag;}}
        public ResolutionEditor(Window owner,int[] rules,int common,int heightLimit,int inputMask,Action<int[],int> save)
        {
            saveDraft=save;
            using(Stream xaml=Assembly.GetExecutingAssembly().GetManifestResourceStream("ResolutionWindow.xaml"))Window=(Window)XamlReader.Load(xaml);
            Window.Resources.MergedDictionaries.Add(owner.Resources);
            Common=(ComboBox)Window.FindName("CommonOutputChoice");ResolutionSettings.Fill(Common,common,false);
            var rows=(StackPanel)Window.FindName("ResolutionRows");
            for(int i=0;i<7;++i) {
                var row=new Grid{MinHeight=42,Margin=new Thickness(12,0,12,0)};row.ColumnDefinitions.Add(new ColumnDefinition{Width=new GridLength(141)});row.ColumnDefinitions.Add(new ColumnDefinition());
                var label=new StackPanel{VerticalAlignment=VerticalAlignment.Center};
                label.Children.Add(new TextBlock{Text=ResolutionSettings.BandLabel(i),FontWeight=FontWeights.SemiBold});
                label.Children.Add(new TextBlock{Text=ResolutionSettings.RangeLabel(i),Style=(Style)owner.FindResource("Subtle")});row.Children.Add(label);
                var combo=new ComboBox{Style=(Style)owner.FindResource("OutputSelect"),MinHeight=34,VerticalAlignment=VerticalAlignment.Center,Margin=new Thickness(0,4,0,4)};
                Choices[i]=combo;Window.RegisterName("ResolutionChoice"+i,combo);
                AutomationProperties.SetName(combo,ResolutionSettings.BandLabel(i)+" 출력 설정");
                ResolutionSettings.Fill(combo,rules[i],true);Grid.SetColumn(combo,1);row.Children.Add(combo);
                rows.Children.Add(new Border{BorderBrush=(Brush)owner.FindResource("Divider"),BorderThickness=new Thickness(0,0,0,i==6?0:1),Child=row});
            }
            ((TextBlock)Window.FindName("ResolutionEligibilityHint")).Text=inputMask==0?
                "현재 보간할 원본 FPS가 선택되지 않아 모든 영상이 원본으로 재생됩니다.":
                heightLimit>0?"현재 "+heightLimit+"p 이하만 보간합니다. 그보다 큰 영상은 개별 설정과 관계없이 원본으로 재생됩니다.":
                "‘보간할 원본 FPS’에서 선택한 영상에만 적용됩니다.";
            ((Button)Window.FindName("ResolutionResetButton")).Click+=delegate {foreach(var combo in Choices)combo.SelectedIndex=0;};
            ((Button)Window.FindName("ResolutionCancelButton")).Click+=delegate {Window.Close();};
            ((Button)Window.FindName("ResolutionSaveButton")).Click+=delegate {
                try {SaveDraft();Window.DialogResult=true;}
                catch(Exception ex) {
                    var notice=(TextBlock)Window.FindName("ResolutionSaveNotice");
                    notice.Text="저장하지 못했습니다. 폴더 쓰기 권한을 확인한 뒤 다시 시도하세요.";
                    notice.Foreground=(Brush)owner.FindResource("Error");notice.ToolTip=ex.Message;
                    AutomationProperties.SetHelpText(notice,ex.Message);
                }
            };
        }
    }
}
