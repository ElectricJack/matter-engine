// A world that OPENS but whose bake fails fatally, for world_stream_tests
// step 10 (a fatal bake sets Failed without deleting runtime entities).
//
// It used to point `WorldDesc::world_name` at a world that simply did not
// exist. Since the project-root layout landed (1fec45ec, 2026-07-19)
// open_world validates the world source up front and refuses with
// "world source not found", so that never produced a session to bake at all.
// A world script that exists and names a module that does not is the same
// test with the failure back where the test wants it: install hits the
// missing-module HARD error and aborts the whole bake.
class FatalBake extends World {
  static roots = [
    { module: 'Task6NoSuchModule',
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] },
  ];
}
