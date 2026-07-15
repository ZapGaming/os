const fs = require('fs');
const path = require('path');

const DB_DIR = path.join(__dirname, 'db');

if (!fs.existsSync(DB_DIR)) {
  fs.mkdirSync(DB_DIR, { recursive: true });
}

/**
 * Minimal synchronous JSON-file-backed key/value store. Good enough for a
 * single-process Discord bot without pulling in a native DB dependency.
 * Every mutation is flushed to disk immediately (atomic rename) so a crash
 * never loses more than the in-flight change.
 */
class JsonStore {
  constructor(fileName, defaultData = {}) {
    this.filePath = path.join(DB_DIR, fileName);
    this.data = this._read(defaultData);
  }

  _read(defaultData) {
    if (!fs.existsSync(this.filePath)) return { ...defaultData };
    try {
      const raw = fs.readFileSync(this.filePath, 'utf8');
      return raw.trim() ? JSON.parse(raw) : { ...defaultData };
    } catch (err) {
      console.error(`Failed to read ${this.filePath}, starting fresh:`, err);
      return { ...defaultData };
    }
  }

  save() {
    const tmpPath = `${this.filePath}.${process.pid}.tmp`;
    fs.writeFileSync(tmpPath, JSON.stringify(this.data, null, 2));
    fs.renameSync(tmpPath, this.filePath);
  }

  get(key) {
    return this.data[key];
  }

  set(key, value) {
    this.data[key] = value;
    this.save();
  }

  has(key) {
    return Object.prototype.hasOwnProperty.call(this.data, key);
  }

  delete(key) {
    delete this.data[key];
    this.save();
  }

  entries() {
    return Object.entries(this.data);
  }
}

module.exports = { JsonStore };
