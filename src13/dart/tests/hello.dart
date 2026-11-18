// hello.dart - D0 do port do Dart: compilado no WSL com
//   dart compile aot-snapshot hello.dart -o HELLO.AOT
// e executado no SODOS com DARTAOT.EXE HELLO.AOT.
import 'dart:async';
import 'dart:io';
import 'dart:isolate';

int fib(int n) => n < 2 ? n : fib(n - 1) + fib(n - 2);

Future<void> main(List<String> args) async {
  print('Ola do Dart ${Platform.version.split(' ').first} no ${Platform.operatingSystem}!');
  print('args: $args');

  final sw = Stopwatch()..start();
  final f = fib(27);
  print('fib(27) = $f em ${sw.elapsedMilliseconds} ms');

  final m = <String, int>{};
  for (var i = 0; i < 20000; i++) {
    m['k${i % 1000}'] = (m['k${i % 1000}'] ?? 0) + i;
  }
  print('mapa: ${m.length} chaves, k7 = ${m['k7']}');

  // Event loop: timer (timerfd + epoll no port) e microtarefas.
  final t0 = DateTime.now();
  await Future.delayed(const Duration(milliseconds: 150));
  final dt = DateTime.now().difference(t0).inMilliseconds;
  print('Future.delayed(150 ms) levou $dt ms');

  // Isolate de verdade (outra thread da VM).
  final r = await Isolate.run(() => fib(20));
  print('Isolate.run(fib(20)) = $r');

  stdout.writeln('stdout ok');
  print('FIM');
}
