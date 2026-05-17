export function Placeholder({ title }: { title: string }) {
  return (
    <div className="page-placeholder">
      <h1 className="page-title">{title}</h1>
      <p className="page-muted">Coming soon</p>
    </div>
  );
}
