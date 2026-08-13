const actionPattern = /^\s*-?\s*uses:\s*([^\s#]+)(?:\s+#.*)?$/gm;
const shaPattern = /^[0-9a-f]{40}$/;

export function unpinnedActions(workflow: string): string[] {
  const failures: string[] = [];
  for (const match of workflow.matchAll(actionPattern)) {
    const reference = match[1];
    if (reference === undefined || reference.startsWith("./")) {
      continue;
    }
    const separator = reference.lastIndexOf("@");
    if (separator < 1 || !shaPattern.test(reference.slice(separator + 1))) {
      failures.push(reference);
    }
  }
  return failures;
}
