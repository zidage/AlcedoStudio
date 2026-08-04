import { redirect } from "next/navigation";

/** Phase 3 MVP lands on the active-run dashboard. */
export default function HomePage(): never {
  redirect("/runs/active");
}
