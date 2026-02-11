void makeTree(const char *fnamein, const char *fnameout)
{
  TTree tin("alcor", "ALCOR");
  tin.ReadFile(fnamein);
  TFile fout(fnameout, "RECREATE");
  tin.Write();
  fout.Close();
}
