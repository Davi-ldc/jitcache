import type { ReactNode } from "react";
import { renderToString } from "react-dom/server";
import products from "./products.json";

type Product = (typeof products)[number];

const price = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" });

function Layout({ title, children }: { title: string; children: ReactNode }) {
  return (
    <html lang="en">
      <head>
        <meta charSet="utf-8" />
        <meta name="viewport" content="width=device-width, initial-scale=1" />
        <title>{`${title} | Acme Outfitters`}</title>
        <link rel="stylesheet" href="/styles.css" />
      </head>
      <body>
        <Header />
        <main>{children}</main>
        <footer>© 2026 Acme Outfitters</footer>
      </body>
    </html>
  );
}

function Header() {
  return (
    <header className="header">
      <a className="logo" href="/">
        Acme Outfitters
      </a>
      <nav>
        <a href="/">Shop</a>
        <a href="/about">About</a>
        <a href="/cart">Cart</a>
      </nav>
    </header>
  );
}

function ProductCard({ product }: { product: Product }) {
  return (
    <article className="product-card">
      <a href={`/products/${product.id}`}>
        <img src={product.image} alt={product.name} width={300} height={300} />
        <h2>{product.name}</h2>
      </a>
      <p className="category">{product.category}</p>
      <p className="price">{price.format(product.price)}</p>
      {product.inStock ? <button>Add to cart</button> : <p className="sold-out">Sold out</p>}
    </article>
  );
}

function ProductList() {
  return (
    <>
      <h1>All products</h1>
      <section className="product-grid">
        {products.map((product) => (
          <ProductCard key={product.id} product={product} />
        ))}
      </section>
    </>
  );
}

function ProductDetails({ product }: { product: Product }) {
  return (
    <article className="product-details">
      <img src={product.image} alt={product.name} width={600} height={600} />
      <div>
        <p className="category">{product.category}</p>
        <h1>{product.name}</h1>
        <p className="price">{price.format(product.price)}</p>
        <p>{product.description}</p>
        <button disabled={!product.inStock}>{product.inStock ? "Add to cart" : "Sold out"}</button>
        <a href="/">Back to all products</a>
      </div>
    </article>
  );
}

function App({ product }: { product?: Product }) {
  return (
    <Layout title={product ? product.name : "Shop"}>
      {product ? <ProductDetails product={product} /> : <ProductList />}
    </Layout>
  );
}

function render(app: ReactNode) {
  return new Response("<!DOCTYPE html>" + renderToString(app), {
    headers: { "Content-Type": "text/html; charset=utf-8" },
  });
}

export default {
  fetch(request: Request) {
    const { pathname } = new URL(request.url);
    if (pathname === "/") return render(<App />);

    const product = products.find((product) => pathname === `/products/${product.id}`);
    if (product) return render(<App product={product} />);

    return new Response("Not found", { status: 404 });
  },
};
