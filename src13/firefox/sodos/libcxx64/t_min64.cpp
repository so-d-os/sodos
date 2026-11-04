extern "C" void k_dbg(const char* s, int n);
int main() {
  k_dbg("MIN64: main\n", 12);
  return 0;
}
