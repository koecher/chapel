record foo { var a : int;  }
proc =(ref x : foo, b) {
  x.a = b.a + 10;
}
var x : foo = new foo();
var y : foo = new foo();
var z : foo = x;
y.a = 1;
x = y;
writeln(x.a);
writeln(z.a);
