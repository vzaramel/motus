-- Motdrasil Example Database Seed
-- Run with: sqlite3 data.db < seed.sql

DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS posts;

-- Products table
CREATE TABLE products (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    price REAL NOT NULL,
    description TEXT,
    category TEXT,
    in_stock INTEGER DEFAULT 1
);

-- Posts table
CREATE TABLE posts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    author TEXT NOT NULL,
    content TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- Sample products
INSERT INTO products (name, price, description, category, in_stock) VALUES
    ('Wireless Headphones', 149.99, 'Premium noise-canceling headphones with 30-hour battery life', 'Electronics', 1),
    ('Mechanical Keyboard', 89.99, 'RGB backlit mechanical keyboard with Cherry MX switches', 'Electronics', 1),
    ('Standing Desk', 449.99, 'Electric height-adjustable desk with memory presets', 'Furniture', 1),
    ('Ergonomic Chair', 299.99, 'Full mesh ergonomic office chair with lumbar support', 'Furniture', 1),
    ('4K Monitor', 379.99, '27-inch 4K IPS monitor with USB-C connectivity', 'Electronics', 1),
    ('Desk Lamp', 59.99, 'LED desk lamp with adjustable color temperature', 'Accessories', 1),
    ('USB-C Hub', 49.99, '7-in-1 USB-C hub with HDMI and SD card reader', 'Electronics', 1),
    ('Laptop Stand', 39.99, 'Aluminum laptop stand with adjustable height', 'Accessories', 1),
    ('Webcam HD', 79.99, '1080p webcam with auto-focus and noise-canceling mic', 'Electronics', 0),
    ('Cable Management Kit', 24.99, 'Complete cable organization system for desks', 'Accessories', 1);

-- Sample posts
INSERT INTO posts (title, author, content) VALUES
    ('Introducing Edge Streaming', 'Motdrasil Team', 'We are excited to announce our new edge streaming capability. Pages are now compiled to bytecode and interpreted at the edge for lightning-fast delivery.'),
    ('Performance Benchmarks', 'Engineering', 'Our latest benchmarks show 3x faster time-to-first-byte compared to traditional server-side rendering. The secret? Streaming bytecode interpretation.'),
    ('New Component System', 'Developer Relations', 'The new component system makes it easy to build reusable UI elements. Define once with defcomp, use everywhere with automatic prop passing.');
