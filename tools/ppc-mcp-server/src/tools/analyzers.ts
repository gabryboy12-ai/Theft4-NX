// Analysis tools for PPC Recompilation
import type {
  PPCIndex,
  SyncPrimitiveAnalysis,
  CallTreeAnalysis,
  KernelTraceUp,
  KernelTraceDown,
  GlobalDependency,
  StubImpact,
  BlockingPath,
  InitValidation,
  AddressDescription,
  RecursiveCallTree,
  VTable,
  VTableEntry,
  ExecutionTrace,
  UpwardPath,
  DownwardNode,
} from '../types.js';

// Memory regions for address classification
const MEMORY_REGIONS = [
  { start: 0x82000000, end: 0x82120000, name: 'PPC image header', description: 'XEX image header and metadata' },
  { start: 0x82120000, end: 0x82A13D5C, name: 'PPC code section', description: 'Recompiled PPC functions' },
  { start: 0x82A13D5C, end: 0x831F0000, name: 'PPC data section', description: 'Global variables, vtables, strings' },
  { start: 0x83000000, end: 0x84000000, name: 'PPC heap/stack', description: 'Dynamic allocations and stack' },
];

// Blocking kernel APIs
const BLOCKING_APIS = new Set([
  'KeWaitForSingleObject', 'KeWaitForMultipleObjects',
  'NtWaitForSingleObject', 'NtWaitForSingleObjectEx', 'NtWaitForMultipleObjects',
  'KeDelayExecutionThread', 'NtDelayExecution',
  'KeAcquireSpinLock', 'ExAcquireReadWriteLockExclusive',
]);

// Sync primitive APIs
const SYNC_CREATE_APIS: Record<string, string> = {
  'KeInitializeEvent': 'event',
  'KeInitializeSemaphore': 'semaphore',
  'KeInitializeMutant': 'mutex',
  'KeInitializeSpinLock': 'spinlock',
};

const SYNC_SIGNAL_APIS = ['KeSetEvent', 'KePulseEvent', 'KeReleaseSemaphore', 'KeReleaseMutant', 'KeReleaseSpinLock'];
const SYNC_WAIT_APIS = ['KeWaitForSingleObject', 'KeWaitForMultipleObjects', 'NtWaitForSingleObject', 'NtWaitForSingleObjectEx'];

export class PPCAnalyzer {
  constructor(private index: PPCIndex) {}

  // Tool 1: sync_primitive_analyzer
  analyzeSyncPrimitive(address: string): SyncPrimitiveAnalysis {
    const normalizedAddr = address.toLowerCase();
    const primitive = this.index.syncPrimitives.get(normalizedAddr);
    
    // Find functions that interact with this address
    const waiters: string[] = [];
    const signalers: string[] = [];
    const creators: string[] = [];
    
    for (const [, func] of this.index.functions) {
      // Check if function accesses this address
      const accessesAddr = 
        func.readsGlobals.some(g => g.address === normalizedAddr) ||
        func.writesGlobals.some(g => g.address === normalizedAddr);
      
      if (accessesAddr) {
        // Check what kernel APIs it calls
        for (const api of func.kernelCalls) {
          if (SYNC_WAIT_APIS.includes(api)) {
            waiters.push(`${func.name} @ ${func.address}`);
          }
          if (SYNC_SIGNAL_APIS.includes(api)) {
            signalers.push(`${func.name} @ ${func.address}`);
          }
          if (SYNC_CREATE_APIS[api]) {
            creators.push(`${func.name} via ${api}`);
          }
        }
      }
    }

    return {
      address: normalizedAddr,
      type: primitive?.type || 'unknown',
      waited_on_by: waiters,
      signaled_by: signalers,
      created_by: creators,
    };
  }

  // Tool 2: call_tree_analyzer
  analyzeCallTree(funcName: string): CallTreeAnalysis | null {
    const func = this.index.functionsByName.get(funcName);
    if (!func) return null;

    const writesGlobals = func.writesGlobals.map(g => ({
      addr: g.address,
      type: this.inferGlobalType(g.address),
    }));

    const syncObjects: { type: string; addr: string }[] = [];
    for (const api of func.kernelCalls) {
      if (SYNC_CREATE_APIS[api]) {
        syncObjects.push({ type: SYNC_CREATE_APIS[api], addr: 'runtime' });
      }
    }

    return {
      function: funcName,
      address: func.address,
      calls: func.calls,
      writes_globals: writesGlobals,
      reads_globals: func.readsGlobals.map(g => g.address),
      creates_sync_objects: syncObjects,
      kernel_apis: func.kernelCalls,
    };
  }

  // Tool 3: kernel_api_tracer - trace UP (callers)
  traceKernelCallers(apiName: string): KernelTraceUp {
    const api = this.index.kernelAPIs.get(apiName);
    const directCallers = api?.directCallers || [];
    
    // Find indirect callers via BFS
    const indirectPaths: string[] = [];
    const visited = new Set<string>();
    
    for (const directCaller of directCallers) {
      this.findCallersRecursive(directCaller, [apiName], visited, indirectPaths, 5);
    }

    return {
      api: apiName,
      direct_callers: directCallers,
      indirect_callers: indirectPaths,
    };
  }

  private findCallersRecursive(
    funcName: string,
    pathSoFar: string[],
    visited: Set<string>,
    results: string[],
    maxDepth: number
  ): void {
    if (maxDepth <= 0 || visited.has(funcName)) return;
    visited.add(funcName);

    const callers = this.index.reverseCallGraph.get(funcName);
    if (!callers || callers.size === 0) {
      if (pathSoFar.length > 1) {
        results.push([funcName, ...pathSoFar].join(' → '));
      }
      return;
    }

    for (const caller of callers) {
      this.findCallersRecursive(caller, [funcName, ...pathSoFar], visited, results, maxDepth - 1);
    }
  }

  // Tool 3: kernel_api_tracer - trace DOWN (to kernel)
  traceToKernel(funcName: string): KernelTraceDown {
    const func = this.index.functionsByName.get(funcName);
    if (!func) return { function: funcName, kernel_apis_reached: [] };

    const results: { api: string; via: string }[] = [];
    const visited = new Set<string>();
    
    this.findKernelApisRecursive(funcName, [], visited, results, 10);

    return {
      function: funcName,
      kernel_apis_reached: results,
    };
  }

  private findKernelApisRecursive(
    funcName: string,
    pathSoFar: string[],
    visited: Set<string>,
    results: { api: string; via: string }[],
    maxDepth: number
  ): void {
    if (maxDepth <= 0 || visited.has(funcName)) return;
    visited.add(funcName);

    const func = this.index.functionsByName.get(funcName);
    if (!func) return;

    const currentPath = [...pathSoFar, funcName];

    // Check direct kernel calls
    for (const api of func.kernelCalls) {
      results.push({
        api,
        via: currentPath.join(' → '),
      });
    }

    // Recurse into called functions
    for (const callee of func.calls) {
      this.findKernelApisRecursive(callee, currentPath, visited, results, maxDepth - 1);
    }
  }

  // Tool 4: global_dependency_tracker
  analyzeGlobal(address: string): GlobalDependency {
    const normalizedAddr = address.toLowerCase();
    const global = this.index.globals.get(normalizedAddr);

    if (!global) {
      return {
        address: normalizedAddr,
        read_by: [],
        warning: 'Global address not found in parsed code',
      };
    }

    let warning: string | undefined;
    
    // Check for potential issues
    if (global.writers.length === 0 && global.readers.length > 0) {
      warning = 'No writers found - may be initialized elsewhere or statically';
    }
    
    // Check if any writers are stubbed
    for (const writer of global.writers) {
      const hook = this.index.hooks.get(writer);
      if (hook?.type === 'GUEST_FUNCTION_STUB') {
        warning = `Writer ${writer} is stubbed - global may not be initialized`;
        break;
      }
    }

    return {
      address: normalizedAddr,
      initialized_by: global.writers.length > 0 
        ? `${global.writers[0]} (and ${global.writers.length - 1} others)` 
        : undefined,
      read_by: global.readers,
      type: this.inferGlobalType(normalizedAddr),
      warning,
    };
  }

  // Tool 5: hook_impact_analyzer
  checkStubImpact(funcName: string): StubImpact {
    const func = this.index.functionsByName.get(funcName);
    if (!func) {
      return {
        function: funcName,
        globals_not_initialized: [],
        functions_that_crash: [],
        sync_objects_not_created: [],
        recommended_action: 'Function not found',
      };
    }

    // Find all globals this function writes (directly and indirectly)
    const globalsNotInit: string[] = [];
    const functionsAffected: string[] = [];
    const syncNotCreated: string[] = [];
    const visited = new Set<string>();
    
    this.collectWrittenGlobals(funcName, globalsNotInit, syncNotCreated, visited);
    
    // Find functions that read these globals
    for (const globalAddr of globalsNotInit) {
      const global = this.index.globals.get(globalAddr);
      if (global) {
        for (const reader of global.readers) {
          if (!functionsAffected.includes(reader)) {
            functionsAffected.push(`${reader} (reads ${globalAddr})`);
          }
        }
      }
    }

    // Determine recommendation
    let recommendation = 'Safe to stub - no critical dependencies found';
    if (globalsNotInit.length > 0) {
      recommendation = 'Hook blocking sub-functions instead, let parent run full initialization';
    }
    if (syncNotCreated.length > 0) {
      recommendation = 'Create sync objects before stubbing, or hook only the blocking wait calls';
    }

    return {
      function: funcName,
      globals_not_initialized: globalsNotInit,
      functions_that_crash: functionsAffected.slice(0, 10),
      sync_objects_not_created: syncNotCreated,
      recommended_action: recommendation,
    };
  }

  private collectWrittenGlobals(
    funcName: string,
    globals: string[],
    syncObjects: string[],
    visited: Set<string>
  ): void {
    if (visited.has(funcName)) return;
    visited.add(funcName);

    const func = this.index.functionsByName.get(funcName);
    if (!func) return;

    // Add directly written globals
    for (const write of func.writesGlobals) {
      if (!globals.includes(write.address)) {
        globals.push(write.address);
      }
    }

    // Check for sync object creation
    for (const api of func.kernelCalls) {
      if (SYNC_CREATE_APIS[api]) {
        syncObjects.push(`${SYNC_CREATE_APIS[api]} via ${api}`);
      }
    }

    // Recurse into called functions
    for (const callee of func.calls) {
      this.collectWrittenGlobals(callee, globals, syncObjects, visited);
    }
  }

  // Tool 6: weak_symbol_checker
  checkHook(funcName: string): {
    function: string;
    hook_active: boolean;
    hook_type?: string;
    host_function?: string;
    file?: string;
    line?: number;
    reason?: string;
    has_weak_symbol: boolean;
  } {
    const func = this.index.functionsByName.get(funcName);
    const hook = this.index.hooks.get(funcName);

    if (!func) {
      return {
        function: funcName,
        hook_active: false,
        reason: 'Function not found in index',
        has_weak_symbol: false,
      };
    }

    if (!hook) {
      return {
        function: funcName,
        hook_active: false,
        reason: 'No hook defined for this function',
        has_weak_symbol: func.hasWeakSymbol,
      };
    }

    // Check if weak symbol might override
    const potentialIssue = func.hasWeakSymbol && hook.type === 'PPC_FUNC';

    return {
      function: funcName,
      hook_active: !potentialIssue,
      hook_type: hook.type,
      host_function: hook.hostFunction,
      file: hook.file,
      line: hook.line,
      reason: potentialIssue 
        ? 'Weak symbol in static library may override hook - use GUEST_FUNCTION_HOOK instead'
        : undefined,
      has_weak_symbol: func.hasWeakSymbol,
    };
  }

  // Tool 7: blocking_path_finder
  findBlockingPath(funcName: string): BlockingPath {
    const func = this.index.functionsByName.get(funcName);
    if (!func) {
      return {
        function: funcName,
        shortest_path: [],
        blocking_api: 'unknown',
        fix_suggestion: 'Function not found',
      };
    }

    // BFS to find shortest path to blocking call
    const queue: { name: string; path: string[] }[] = [{ name: funcName, path: [funcName] }];
    const visited = new Set<string>();

    while (queue.length > 0) {
      const current = queue.shift()!;
      if (visited.has(current.name)) continue;
      visited.add(current.name);

      const currentFunc = this.index.functionsByName.get(current.name);
      if (!currentFunc) continue;

      // Check for blocking API calls
      for (const api of currentFunc.kernelCalls) {
        if (BLOCKING_APIS.has(api)) {
          return {
            function: funcName,
            shortest_path: [...current.path, api],
            blocking_api: api,
            fix_suggestion: `Hook ${current.name} to bypass blocking call, or signal the waited object before`,
          };
        }
      }

      // Add callees to queue
      for (const callee of currentFunc.calls) {
        if (!visited.has(callee)) {
          queue.push({ name: callee, path: [...current.path, callee] });
        }
      }
    }

    return {
      function: funcName,
      shortest_path: [],
      blocking_api: 'none',
      fix_suggestion: 'No blocking calls found in reachable code',
    };
  }

  // Tool 8: init_sequence_validator
  validateInit(funcName: string): InitValidation {
    const func = this.index.functionsByName.get(funcName);
    if (!func) {
      return {
        function: funcName,
        globals_initialized: [],
        globals_missing: [],
      };
    }

    const globalsInit: string[] = [];
    const globalsMissing: string[] = [];
    const visited = new Set<string>();
    let blockedAt: string | undefined;
    let blockingReason: string | undefined;

    // Collect all globals that would be initialized
    this.collectInitializedGlobals(funcName, globalsInit, visited);

    // Check for blocking points
    const blockingPath = this.findBlockingPath(funcName);
    if (blockingPath.blocking_api !== 'none') {
      blockedAt = blockingPath.shortest_path[blockingPath.shortest_path.length - 2];
      blockingReason = `${blockingPath.blocking_api} call`;
    }

    // Check if any stubbed functions would prevent initialization
    for (const callee of func.calls) {
      const hook = this.index.hooks.get(callee);
      if (hook?.type === 'GUEST_FUNCTION_STUB') {
        // Find what globals the stubbed function would have written
        const stubbedFunc = this.index.functionsByName.get(callee);
        if (stubbedFunc) {
          for (const write of stubbedFunc.writesGlobals) {
            if (!globalsMissing.includes(write.address)) {
              globalsMissing.push(write.address);
            }
          }
        }
      }
    }

    return {
      function: funcName,
      blocked_at: blockedAt,
      blocking_reason: blockingReason,
      globals_initialized: globalsInit.slice(0, 20),
      globals_missing: globalsMissing,
    };
  }

  private collectInitializedGlobals(
    funcName: string,
    globals: string[],
    visited: Set<string>
  ): void {
    if (visited.has(funcName)) return;
    visited.add(funcName);

    const func = this.index.functionsByName.get(funcName);
    if (!func) return;

    for (const write of func.writesGlobals) {
      if (!globals.includes(write.address)) {
        globals.push(write.address);
      }
    }

    for (const callee of func.calls) {
      // Skip if callee is stubbed
      const hook = this.index.hooks.get(callee);
      if (hook?.type !== 'GUEST_FUNCTION_STUB') {
        this.collectInitializedGlobals(callee, globals, visited);
      }
    }
  }

  // Tool 9: vtable_inspector
  inspectVTable(address: string): VTable | null {
    const normalizedAddr = address.toLowerCase();
    
    // VTables are typically in the data section with function pointers
    const global = this.index.globals.get(normalizedAddr);
    if (!global) return null;

    // This is a simplified vtable detection
    // In practice, we'd need to analyze memory layout
    const entries: VTableEntry[] = [];
    const usedBy: string[] = [];

    // Find functions that read this address (vtable users)
    for (const reader of global.readers) {
      usedBy.push(reader);
    }

    return {
      address: normalizedAddr,
      entries,
      initializedBy: global.writers[0],
      usedBy,
    };
  }

  // Tool 10: ppc_memory_map
  describeAddress(address: string): AddressDescription {
    const addr = parseInt(address, 16);
    const normalizedAddr = address.toLowerCase();

    // Find region
    let region = 'Unknown';
    let description = 'Address not in known PPC memory range';
    
    for (const r of MEMORY_REGIONS) {
      if (addr >= r.start && addr < r.end) {
        region = r.name;
        description = r.description;
        break;
      }
    }

    // Check if it's a known function
    const func = this.index.functions.get(normalizedAddr);
    if (func) {
      return {
        address: normalizedAddr,
        region,
        type: 'function',
        used_by: func.calledBy.slice(0, 10),
      };
    }

    // Check if it's a known global
    const global = this.index.globals.get(normalizedAddr);
    if (global) {
      return {
        address: normalizedAddr,
        region,
        type: this.inferGlobalType(normalizedAddr),
        created_by: global.writers[0],
        used_by: global.readers.slice(0, 10),
      };
    }

    return {
      address: normalizedAddr,
      region,
      type: description,
    };
  }

  // Tool 11: recursive_call_tree
  buildRecursiveCallTree(funcName: string, maxDepth: number = 5): RecursiveCallTree | null {
    const func = this.index.functionsByName.get(funcName);
    if (!func) return null;

    return this.buildCallTreeRecursive(funcName, new Set(), maxDepth);
  }

  private buildCallTreeRecursive(
    funcName: string,
    visited: Set<string>,
    remainingDepth: number
  ): RecursiveCallTree {
    const func = this.index.functionsByName.get(funcName);
    
    const tree: RecursiveCallTree = {
      function: funcName,
      address: func?.address || 'unknown',
      depth: 0,
      calls: [],
      globals_accessed: [],
      kernel_apis: func?.kernelCalls || [],
    };

    if (!func || remainingDepth <= 0 || visited.has(funcName)) {
      return tree;
    }

    visited.add(funcName);

    // Collect globals
    tree.globals_accessed = [
      ...func.readsGlobals.map(g => `R:${g.address}`),
      ...func.writesGlobals.map(g => `W:${g.address}`),
    ].slice(0, 10);

    // Recurse into calls
    for (const callee of func.calls) {
      const childTree = this.buildCallTreeRecursive(callee, new Set(visited), remainingDepth - 1);
      childTree.depth = tree.depth + 1;
      tree.calls.push(childTree);
    }

    return tree;
  }

  // Helper: infer global type
  private inferGlobalType(address: string): string | undefined {
    const addr = parseInt(address, 16);
    
    // Heuristics based on address patterns
    if (addr >= 0x82010000 && addr < 0x82020000) {
      return 'vtable_pointer';
    }
    if (addr >= 0x828D0000 && addr < 0x82900000) {
      return 'game_state';
    }
    if (addr >= 0x83100000 && addr < 0x83200000) {
      return 'dynamic_object';
    }
    
    return undefined;
  }

  // Utility: search functions
  searchFunctions(pattern: string): string[] {
    const regex = new RegExp(pattern, 'i');
    const results: string[] = [];
    
    for (const [name] of this.index.functionsByName) {
      if (regex.test(name)) {
        results.push(name);
      }
    }
    
    return results.slice(0, 50);
  }

  // Utility: list all hooks
  listHooks(): { name: string; type: string; host?: string }[] {
    const hooks: { name: string; type: string; host?: string }[] = [];
    
    for (const [name, hook] of this.index.hooks) {
      hooks.push({
        name,
        type: hook.type,
        host: hook.hostFunction,
      });
    }
    
    return hooks;
  }

  // Tool: execution_trace - Trace execution paths up to entry and down to callees
  executionTrace(
    funcName: string,
    direction: 'up' | 'down' | 'both',
    maxDepth: number = 50
  ): ExecutionTrace {
    const func = this.index.functionsByName.get(funcName);
    if (!func) {
      return {
        function: funcName,
        error: 'Function not found',
        upward_trace: [],
        downward_trace: [],
      };
    }

    const result: ExecutionTrace = {
      function: funcName,
      address: func.address,
      upward_trace: [],
      downward_trace: [],
    };

    // Trace upward to entry point (xstart or root callers)
    if (direction === 'up' || direction === 'both') {
      result.upward_trace = this.traceToEntryPoint(funcName, maxDepth);
      result.entry_points = result.upward_trace
        .filter(path => path.is_entry_point)
        .map(path => path.path[0]);
    }

    // Trace downward to all callees
    if (direction === 'down' || direction === 'both') {
      result.downward_trace = this.traceCallees(funcName, maxDepth);
      result.leaf_functions = result.downward_trace
        .filter(item => item.is_leaf)
        .map(item => item.function);
      result.kernel_apis_reached = result.downward_trace
        .filter(item => item.kernel_apis.length > 0)
        .flatMap(item => item.kernel_apis)
        .filter((api, idx, arr) => arr.indexOf(api) === idx); // unique
    }

    return result;
  }

  private traceToEntryPoint(
    funcName: string,
    maxDepth: number
  ): UpwardPath[] {
    const paths: UpwardPath[] = [];
    const visited = new Set<string>();
    
    // Known entry points
    const ENTRY_POINTS = new Set([
      'xstart', 'XapiThreadStartup', '__entry', 'main',
      'sub_82100000', // Common XEX entry
    ]);

    const findPaths = (
      current: string,
      pathSoFar: string[],
      depth: number
    ): void => {
      if (depth > maxDepth) return;
      
      const currentPath = [current, ...pathSoFar];
      const callers = this.index.reverseCallGraph.get(current);
      
      // Check if current is an entry point
      if (ENTRY_POINTS.has(current) || !callers || callers.size === 0) {
        paths.push({
          path: currentPath,
          depth: currentPath.length,
          is_entry_point: ENTRY_POINTS.has(current) || !callers || callers.size === 0,
          entry_type: ENTRY_POINTS.has(current) ? 'known_entry' : 'root_caller',
        });
        return;
      }

      // Limit paths to avoid explosion
      if (paths.length >= 20) return;

      for (const caller of callers) {
        // Avoid cycles
        if (pathSoFar.includes(caller)) continue;
        
        // Use path-local visited to allow multiple paths
        findPaths(caller, currentPath, depth + 1);
      }
    };

    findPaths(funcName, [], 0);
    
    // Sort by shortest path first
    return paths.sort((a, b) => a.depth - b.depth).slice(0, 20);
  }

  private traceCallees(
    funcName: string,
    maxDepth: number
  ): DownwardNode[] {
    const nodes: DownwardNode[] = [];
    const visited = new Set<string>();

    const traverse = (current: string, depth: number): void => {
      if (depth > maxDepth || visited.has(current)) return;
      visited.add(current);

      const func = this.index.functionsByName.get(current);
      const callees = func?.calls || [];
      const kernelApis = func?.kernelCalls || [];
      
      nodes.push({
        function: current,
        address: func?.address || 'unknown',
        depth,
        callees: callees.slice(0, 20), // Limit for readability
        kernel_apis: kernelApis,
        is_leaf: callees.length === 0,
        is_hooked: this.index.hooks.has(current),
        hook_type: this.index.hooks.get(current)?.type,
      });

      for (const callee of callees) {
        traverse(callee, depth + 1);
      }
    };

    traverse(funcName, 0);
    return nodes;
  }

  // Utility: list kernel APIs
  listKernelAPIs(category?: string): { name: string; category: string; callers: number; blocking: boolean }[] {
    const apis: { name: string; category: string; callers: number; blocking: boolean }[] = [];
    
    for (const [, api] of this.index.kernelAPIs) {
      if (!category || api.category === category) {
        apis.push({
          name: api.name,
          category: api.category,
          callers: api.directCallers.length,
          blocking: api.isBlocking,
        });
      }
    }
    
    return apis.sort((a, b) => b.callers - a.callers);
  }
}
