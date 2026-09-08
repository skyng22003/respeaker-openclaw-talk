export class BoundedQueue<T> {
  readonly capacity: number;
  readonly #values: T[] = [];

  constructor(capacity: number) {
    if (!Number.isInteger(capacity) || capacity <= 0) {
      throw new RangeError("Queue capacity must be a positive integer");
    }
    this.capacity = capacity;
  }

  get size(): number {
    return this.#values.length;
  }

  push(value: T): { dropped?: T } {
    if (this.#values.length < this.capacity) {
      this.#values.push(value);
      return {};
    }

    const dropped = this.#values.shift() as T;
    this.#values.push(value);
    return { dropped };
  }

  shift(): T | undefined {
    return this.#values.shift();
  }

  clear(): void {
    this.#values.length = 0;
  }
}
