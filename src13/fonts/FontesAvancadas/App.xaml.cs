using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace FontesAvancadas;

public partial class App : Application
{
    public App()
    {
        DispatcherUnhandledException += OnUnhandled;
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            File.AppendAllText(@"C:\sodos\fontes-avancadas.log", e.ExceptionObject + "\r\n");
    }

    private void OnUnhandled(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        File.AppendAllText(@"C:\sodos\fontes-avancadas.log", e.Exception + "\r\n");
        MessageBox.Show(e.Exception.Message, "Fontes Avançadas");
        e.Handled = true;
    }
}
