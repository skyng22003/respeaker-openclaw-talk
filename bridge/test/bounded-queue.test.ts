import { describe, expect, it } from "vitest";

import { BoundedQueue } from "../src/bounded-queue.js";

describe("BoundedQueue", () => {
  it("keeps a fixed capacity and drops the oldest value", () => {
    const queue = new BoundedQueue<number>(2);

    expect(queue.push(1)).toEqual({});
    expect(queue.push(2)).toEqual({});
    expect(queue.push(3)).toEqual({ dropped: 1 });
    expect(queue.size).toBe(2);
  });

  it("maintains FIFO order among retained values", () => {
    const queue = new BoundedQueue<string>(2);
    queue.push("stale");
    queue.push("middle");
    queue.push("newest");

    expect(queue.shift()).toBe("middle");
    expect(queue.shift()).toBe("newest");
    expect(queue.shift()).toBeUndefined();
  });

  it("reports eviction even when the dropped value is undefined", () => {
    const queue = new BoundedQueue<undefined>(1);
    queue.push(undefined);

    expect(queue.push(undefined)).toEqual({ dropped: undefined });
    expect(Object.hasOwn(queue.push(undefined), "dropped")).toBe(true);
  });

  it("clears every queued value", () => {
    const queue = new BoundedQueue<number>(3);
    queue.push(1);
    queue.push(2);

    queue.clear();

    expect(queue.size).toBe(0);
    expect(queue.shift()).toBeUndefined();
  });

  it("rejects non-positive or fractional capacities", () => {
    expect(() => new BoundedQueue(0)).toThrow();
    expect(() => new BoundedQueue(1.5)).toThrow();
  });
});
