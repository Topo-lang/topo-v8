export function publicApi(x: number): number {
    return x * 2;
}

export function internalHelper(x: number): number {
    return x + 1;
}

export function privateImpl(x: number): number {
    return x - 1;
}

export const PUBLIC_CONST = 42;

function notListed(): void {
    // Not in visibility map — left untouched.
}
