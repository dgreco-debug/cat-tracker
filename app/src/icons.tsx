// Inline stroke icons (24px grid).
const S = (p: { d: string | string[]; fill?: boolean }) => (
  <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    {(Array.isArray(p.d) ? p.d : [p.d]).map((d) => <path d={d} />)}
  </svg>
);

export const IconMap = () => <S d={['M9 4 3 6v14l6-2 6 2 6-2V4l-6 2-6-2z', 'M9 4v14', 'M15 6v14']} />;
export const IconTag = () => <S d={['M4 12a8 8 0 1 0 16 0', 'M8 12a4 4 0 0 0 8 0', 'M12 4v4', 'M12 16v4']} />;
export const IconChat = () => <S d={['M21 12a8 8 0 0 1-11.6 7.1L4 20l1-4.6A8 8 0 1 1 21 12z']} />;
export const IconList = () => <S d={['M8 6h13', 'M8 12h13', 'M8 18h13', 'M3 6h.01', 'M3 12h.01', 'M3 18h.01']} />;
export const IconBluetooth = () => <S d={['m7 7 10 10-5 5V2l5 5L7 17']} />;
export const IconUsb = () => <S d={['M12 2v14', 'M8 6l4-4 4 4', 'M7 11v2a5 5 0 0 0 10 0v-2', 'M12 18v4']} />;
export const IconTarget = () => <S d={['M12 2v3', 'M12 19v3', 'M2 12h3', 'M19 12h3', 'M12 7a5 5 0 1 0 0 10 5 5 0 0 0 0-10z']} />;
export const IconMe = () => <S d={['M12 3l7 18-7-4-7 4 7-18z']} />;
export const IconLayers = () => <S d={['m12 2 10 5-10 5L2 7l10-5z', 'm2 17 10 5 10-5', 'm2 12 10 5 10-5']} />;
export const IconRadio = () => <S d={['M4.9 19.1a10 10 0 0 1 0-14.2', 'M19.1 4.9a10 10 0 0 1 0 14.2', 'M8.5 15.5a5 5 0 0 1 0-7', 'M15.5 8.5a5 5 0 0 1 0 7', 'M12 12h.01']} />;
export const IconSend = () => <S d={['M22 2 11 13', 'M22 2l-7 20-4-9-9-4 20-7z']} />;
export const IconPlay = () => <S d={['M5 3l14 9-14 9V3z']} />;

/** The cat marker: two eyes catching the light. */
export const CatEyes = ({ size = 44 }: { size?: number }) => (
  <svg width={size} height={size} viewBox="0 0 44 44" aria-hidden="true">
    <circle cx="22" cy="22" r="20" fill="#0f1c2e" stroke="#f2b33d" stroke-width="2" />
    <g class="eyes">
      <path d="M9.5 21c2-4 8-4 10 0-2 4-8 4-10 0z" fill="#f2b33d" />
      <path d="M24.5 21c2-4 8-4 10 0-2 4-8 4-10 0z" fill="#f2b33d" />
      <path d="M14.5 18v6M29.5 18v6" stroke="#0f1c2e" stroke-width="2.2" stroke-linecap="round" />
    </g>
  </svg>
);
