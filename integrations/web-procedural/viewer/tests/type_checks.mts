// Compile-time contract between the two packages, which deliberately do not
// import each other: the runtime must satisfy the structural DemoRuntime the
// viewer's demo session is written against.
import {ProceduralRuntime} from '../../runtime/src/index.mjs';
import type {DemoRuntime} from '../src/demo/session.mjs';

declare const runtime: ProceduralRuntime;
export const demoRuntime: DemoRuntime = runtime;
