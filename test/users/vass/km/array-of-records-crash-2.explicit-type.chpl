// This is a copy of array-of-records-crash-2.chpl but with
// the explicit type of Vertices uncommented.
// This is an extract from KM's code, May 2011.

const N_VERTICES = 64;
const vertex_domain = {1..N_VERTICES} ;

record vertex_struct {

  // constructor
  proc vertex_struct(nd: domain(1)) {
    this.nd = nd;
    vlock$ = true;
  }

  var nd: domain(1);
  var vlock$: sync bool;

}

// bug note: uncommenting the type of Vertices eliminates the symptoms
var Vertices : [vertex_domain] vertex_struct
  = [i in vertex_domain] new vertex_struct(nd={1..5});
