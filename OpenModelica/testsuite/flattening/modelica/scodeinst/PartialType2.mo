// name: PartialType2
// keywords:
// status: incorrect
// cflags: -d=newInst
//

partial model A
  Real x;
end A;

model B = A;

model PartialType2
  B b;
end PartialType2;

// Result:
// Error processing file: PartialType2.mo
// [flattening/modelica/scodeinst/PartialType2.mo:11:1-11:12:writable] Notification: From here:
// [flattening/modelica/scodeinst/PartialType2.mo:14:3-14:6:writable] Error: Component âbâ has partial type âBâ.
//
// # Error encountered! Exiting...
// # Please check the error message and the flags.
//
// Execution failed!
// endResult
